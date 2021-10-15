#include <glib.h>
#include <stdio.h>
#include <errno.h>
#include <getopt.h>

#include "debug.h"

#define PROG_VERSION       "0.1a"

#define SWE_LOWER_OE       "\xc3\xb6"
#define SWE_LOWER_AO       "\xc3\xa5"

#define CARD_IDSTR_BEGIN_PFX       "Nya k"SWE_LOWER_OE"p f"SWE_LOWER_OE"r "
#define CARD_IDSTR_END_PFX         "Summa nya k"SWE_LOWER_OE"p för "
#define INBET_IDSTR_PFX            " Inbetalningar"
#define EXTRAKORT_PFX              "Extrakort som slutar p"SWE_LOWER_AO" "
#define PAGE_IDSTR_PFX             "Sida "
#define OCR_IDSTR                  "OCR: "
#define DUE_DATE_IDSTR             "F"SWE_LOWER_OE"rfallodag"

#define DEFAULT_LOCATION_FILE      "locations.txt"
#define DEFAULT_LINE_SPLIT_WIDTH    80
#define MIN_LINE_SPLIT_WIDTH        10

struct transaction {
  GDateTime *date;
  GDateTime *process_date;
  gdouble value_sek;
  gchar *location;
  gchar *details;
};

struct amex_card {
 gchar *holder;
 gchar *suffix;
 GPtrArray *transactions;
};

struct statistics {
  gint total_lines;
  gint skipped_lines;
  guint transaction_count;
};

struct prog_options {
  gchar *outfile;
  gchar *infile;
  gint line_split_width;
  gchar *location_file;
};

struct prog_state {
  struct prog_options opts;
  GHashTable *loc_hash;
  struct amex_card *curr_card;
  struct statistics stats;
  guint idx;
  gchar *faktura_ocr;
  GDateTime *faktura_due_date;
  GPtrArray *cards;
  GPtrArray *lines;
};

DEFINE_GQUARK("amex_parser");

const gchar *prog_name;

static gchar *
format_dt(GDateTime *dt)
{
  static gchar buffer[16];

  if (!dt) {
    g_snprintf(buffer, sizeof(buffer), "<invalid>");
    return buffer;
  }

  g_snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
             g_date_time_get_year(dt),
             g_date_time_get_month(dt),
             g_date_time_get_day_of_month(dt));

  return buffer;
}

static void
free_transaction_entry(gpointer data)
{
  struct transaction *ent = (struct transaction *) data;

  if (!ent) {
    return;
  }

  if (ent->date) {
    g_date_time_unref(ent->date);
  }
  if (ent->process_date) {
    g_date_time_unref(ent->process_date);
  }
  g_free(ent->details);
  g_free(ent->location);
  g_free(ent);
}

static void
free_amex_card(gpointer data)
{
  struct amex_card *card = (struct amex_card *) data;

  if (!card) {
    return;
  }

  if (card->transactions) {
    g_ptr_array_free(card->transactions, TRUE);
  }
  g_free(card->holder);
  g_free(card);
}

static const gchar *
print_amex_card(const struct amex_card *card)
{
  static gchar buffer[256];
  gchar suffix_str[32] = "";

  if (!card || !card->holder) {
    return "<invalid card>";
  }

  if (card->suffix) {
    g_snprintf(suffix_str, sizeof(suffix_str), "-%s", card->suffix);
  }

  g_snprintf(buffer, sizeof(buffer), "%s%s",
             card->holder, suffix_str);

  return buffer;
}

static struct amex_card *
alloc_amex_card(const gchar *holder, const gchar *suffix)
{
  struct amex_card *card;

  g_assert(holder);

  card = g_malloc0(sizeof(*card));
  card->holder = g_strdup(holder);
  card->suffix = g_strdup(suffix);
  card->transactions = g_ptr_array_new_with_free_func(free_transaction_entry);
  g_message("Allocated new %sAmex card %s for %s",
            card->suffix ? "Extra " : "",
            card->suffix ? card->suffix : "", card->holder);

  return card;
}

static gboolean
parse_page_num(const gchar *str, gint *page, gint *total_pages, GError **err)
{
  gint tc;

  g_assert(str);
  g_assert(total_pages);
  g_assert(page);

  if ((tc = sscanf(str, PAGE_IDSTR_PFX"%d av %d", page, total_pages)) < 0) {
    SET_GERROR(err, -1, "could not parse page number: %s", g_strerror(errno));
    return FALSE;
  } else if (tc != 2) {
    SET_GERROR(err, -1,
               "invalid token count when parsing page (got %d, need 2)", tc);
    return FALSE;
  }

  return TRUE;
}

static void
combine_columns(GPtrArray *results, GList **lhs, GList **rhs)
{
  gint i;

  g_assert(results);
  g_assert(lhs);
  g_assert(rhs);

  for (i = 0; i < 2; i++) {
    GList **l = i == 0 ? lhs : rhs;
    GList *node;
    gint j;

    for (node = *l, j = 0; node; node = node->next, j++) {
      g_ptr_array_add(results, g_strdup((gchar *) node->data));
    }
    g_list_free_full(*l, g_free);
    *l = NULL;
    g_message("[%s] Added %d entries", i == 0 ? "LHS" : "RHS", j);
  }
}

static gboolean
split_lines_file(const gchar *filename, gint split_width,
                 GPtrArray *lines, GError **err)
{
  gchar *buffer = NULL;
  gboolean ret = FALSE;
  gint page = 0;
  gint last_page = 0;
  gint page_total = 0;
  guint i;
  gsize flen = 0;
  gchar **splits = NULL;

  GList *lhs = NULL;
  GList *rhs = NULL;

  g_assert(filename);
  g_assert(lines);

  /* 0. Read the file */
  if (!g_file_get_contents(filename, &buffer, &flen, err)) {
    return FALSE;
  }

  /* 1. Split into lines */
  splits = g_strsplit(buffer, "\n", -1);

  for (i = 0; splits[i]; i++) {
    gchar *l = splits[i];
    const gchar *tmp = g_strstr_len(l, -1, PAGE_IDSTR_PFX);
    gchar *tmp_lhs = NULL;
    gchar *tmp_rhs = NULL;
    gint slen;

    if (!tmp && !page_total) {
      /* Discard everything until we find the page identifier */
      continue;
    } else if (tmp) {
      /* Page indicator */
      if (!parse_page_num(tmp, &page, &page_total, err)) {
        goto out;
      }

      if (page > 1 && page != last_page) {
        /* Handle next page here */
        combine_columns(lines, &lhs, &rhs);
        last_page = page;
      }
      g_message("Processing page %d of %d...", page, page_total);
      continue;
    }

    slen = strlen(l);
    g_strdelimit(l, ";", '?');

    if (slen >= split_width) {
      l[split_width - 1] = '\0';
      tmp_lhs = g_strdup(g_strstrip(l));
      tmp_rhs = g_strdup(g_strstrip(l + split_width));
    } else {
      tmp_lhs = g_strdup(g_strstrip(l));
    }

    if (strlen(tmp_lhs)) {
      lhs = g_list_append(lhs, tmp_lhs);
    } else {
      g_free(tmp_lhs);
    }

    if (tmp_rhs && strlen(tmp_rhs)) {
      rhs = g_list_append(rhs, tmp_rhs);
    } else {
      g_free(tmp_rhs);
    }
  }

  /* If we didn't find a page total then this is probably not an Amex faktura */
  if (!page_total) {
    SET_GERROR(err, -1,
               "could not find page identifier (is this an Amex bill?)");
    goto out;
  }

  g_message("Read %zi byte(s), %d pages and added %d line(s) from '%s'",
            flen, page_total, lines->len, filename);
  ret = TRUE;

out:
  g_strfreev(splits);
  g_free(buffer);
  g_list_free_full(lhs, g_free);
  g_list_free_full(rhs, g_free);

  if (!ret) {
    g_prefix_error(err, "L%d: ", i);
  }

  return ret;
}

static gboolean
handle_card_change(struct prog_state *state, const gchar *holder,
                   GError **err)
{
  guint i;
  gchar *hldr_str = g_strdup(holder);
  gchar *eptr = g_strstr_len(hldr_str, -1, EXTRAKORT_PFX);
  gchar *suffix = NULL;

  if (eptr) {
    *eptr = '\0';
    suffix = g_strstrip(eptr + strlen(EXTRAKORT_PFX));
  }

  g_strstrip(hldr_str);

  for (i = 0; i < state->cards->len; i++) {
    struct amex_card *c = g_ptr_array_index(state->cards, i);

    if (!g_strcmp0(c->holder, hldr_str)) {
      if (!suffix || !g_strcmp0(suffix, c->suffix)) {
        g_message("Using existing card '%s'", print_amex_card(c));
        state->curr_card = c;
        goto out;
      }
    }
  }

  /* New card */
  state->curr_card = alloc_amex_card(g_strstrip(hldr_str), suffix);
  g_ptr_array_add(state->cards, state->curr_card);
  /* fall through */

out:
  g_free(hldr_str);

  return TRUE;
}

#define DATE_STR_LEN 9

static GDateTime *
parse_amex_date(const gchar *str, GError **err)
{
  GDateTime *res;
  gint year = 0;
  gint month = 0;
  gint day = 0;
  gint tc;

  /* Amex date format is DD.MM.YY */
  if ((tc = sscanf(str, "%02d.%02d.%02d",
                  &day, &month, &year)) != 3) {
    SET_GERROR(err, -1, "invalid date format, %s",
               tc < 0 ? g_strerror(errno) : "too few tokens");
    return NULL;
  }

  if ((res = g_date_time_new_local(year + 2000,
                                   month,
                                   day, 12, 00, 00.00)) == NULL) {
    /* Not a valid date */
    SET_GERROR(err, -1, "could not parse date");
  }

  return res;
}

static gboolean
is_amex_transaction(const gchar *line, GDateTime **tdate, GDateTime **pdate)
{
  GDateTime *td = NULL;
  GDateTime *pd = NULL;
  guint offs;
  gint i;

  g_assert(line);

  if (strlen(line) < DATE_STR_LEN * 2) {
    /* Line is too short */
    return FALSE;
  }

  /* 08.06.21 08.06.21 */
  for (i = 0, offs = 0; i < 2; i++, offs += DATE_STR_LEN) {
    GDateTime **ptr = i == 0 ? &td : &pd;

    if ((*ptr = parse_amex_date(line + offs, NULL)) == NULL) {
      /* Not a valid date */
      goto out;
    }
  }

out:
  if (!td || !pd) {
    g_clear_pointer(&td, g_date_time_unref);
    g_clear_pointer(&pd, g_date_time_unref);
    /* not an AMEX transaction */

    return FALSE;
  }

  g_assert(td && pd);
  if (tdate) {
    *tdate = g_date_time_ref(td);
  }
  if (pdate) {
    *pdate = g_date_time_ref(pd);
  }

  g_date_time_unref(td);
  g_date_time_unref(pd);

  return TRUE;
}

static gboolean
parse_transaction_amount(const gchar *str, gdouble *result, GError **err)
{
  gint i;
  gint j;
  gchar sbuff[64];
  gchar *eptr = NULL;

  g_assert(str);
  g_assert(result);

  for (i = 0, j = 0; str[i] && j < sizeof(sbuff) - 1; i++) {

    if (!g_ascii_isdigit(str[i])) {
      if (!i) {
        if (str[i] != '-') {
          SET_GERROR(err, -1, "value '%s' is malformed", str);
          return FALSE;
        }
        /* negative transaction */
      } else if (str[i] == '.') {
        continue;
      } else if (str[i] == ',') {
        sbuff[j++] = '.';
        continue;
      } else if (str[i] == ' ') {
        break;
      } else {
        SET_GERROR(err, -1, "invalid character 0x%x in value (str=%s)", str[i], str);
        return FALSE;
      }
    }
    sbuff[j++] = str[i];
  }

  sbuff[j] = '\0';
  *result = g_ascii_strtod(sbuff, &eptr);

  if (errno) {
    SET_GERROR(err, -1, "could not convert amount '%s' to float", sbuff);
    return FALSE;
  } else if (eptr && strlen(eptr)) {
    SET_GERROR(err, -1, "got garbage after amount");
    return FALSE;
  }

  return TRUE;
}

static gboolean
parse_transaction_details(struct prog_state *state, const gchar *str,
                          struct transaction *t,
                          GError **err)
{
  GString *gs = NULL;
  gchar *loc_str = NULL;
  gchar *ldup = NULL;
  gchar **splits = NULL;
  guint tc;
  guint i;

  g_assert(state);
  g_assert(str);
  g_assert(t);
  g_assert(state->loc_hash);

  ldup = g_strstrip(g_strdup(str));

  /* Algorithm is as follows:
   *  - Take the following line and check against the location hash. If a match
   *    is made, then use this as the location, the remainder as the details.
   *  - If no match, take the last string
   */
  if (state->idx + 1 < state->lines->len + 1) {
    gchar *tmp = (gchar *) g_ptr_array_index(state->lines, state->idx + 1);

    if ((loc_str = g_strdup(g_hash_table_lookup(state->loc_hash,
                                                tmp))) != NULL) {
      /* Got a location match! Nice! */
      state->idx++;
    }
  }

  splits = g_strsplit(ldup, " ", -1);
  if ((tc = g_strv_length(splits)) <= 1) {
    g_warning("L%d: Very weird line with no spaces (%s)", state->idx, ldup);
    t->details = g_strdup(ldup);
    t->location = loc_str;
    return TRUE;
  }

  g_debug("Remaining line: '%s'  tc=%u", ldup, g_strv_length(splits));
  gs = g_string_new(NULL);
  /* Check if the last token is the location */
  if (!loc_str) {
    loc_str = g_strdup(g_hash_table_lookup(state->loc_hash, splits[tc -1]));
  }
  for (i = 0; i < tc; i++) {
    if (!strlen(splits[i])) {
      continue;
    } else if (loc_str && i == tc - 1 && !g_strcmp0(loc_str, splits[i])) {
      continue;
    }
    g_string_append_printf(gs, "%s%s", gs->len ? " " : "", splits[i]);
  }

  g_strfreev(splits);
  g_free(ldup);
  t->location = loc_str;
  t->details = g_string_free(gs, FALSE);

  return TRUE;

}

static gboolean
process_transaction_line(struct prog_state *state, const gchar *line,
                         GError **err)
{
  struct transaction *t;
  gchar *tmp;
  gchar *ldup = NULL;

  g_assert(state);
  g_assert(line);

  if (!state->curr_card) {
    /* This should probably be an error ... */
    g_warning("Transaction without a current card!");
    return TRUE;
  }

  t = g_malloc0(sizeof(*t));
  if (!is_amex_transaction(line, &t->date, &t->process_date)) {
    SET_GERROR(err, -1, "not a valid AMEX transaction");
    goto out_fail;
  }

  g_assert(strlen(line) >= DATE_STR_LEN * 2);
  ldup = g_strdup(line + DATE_STR_LEN * 2);

  /* Get the value of the transaction */
  if ((tmp = g_strrstr(ldup, " ")) == NULL) {
    SET_GERROR(err, -1, "malformed line, missing amount separator");
    goto out_fail;
  }

  /* Parse the value */
  if (!parse_transaction_amount(tmp + 1, &t->value_sek, err)) {
    g_prefix_error(err, "process amount: ");
    goto out_fail;
  }

  *tmp = '\0'; /* Truncate the amount part */
  if (!parse_transaction_details(state, ldup, t, err)) {
    g_prefix_error(err, "parse details: ");
    goto out_fail;
  }

  g_message("Transaction for '%s', location=%s on %s for %.2f SEK, details: '%s'",
            state->curr_card->holder,
            t->location ? t->location : "unknown",
            format_dt(t->date), t->value_sek, t->details);

  g_free(ldup);
  g_ptr_array_add(state->curr_card->transactions, t);
  state->stats.transaction_count++;

  return TRUE;

out_fail:
  g_free(ldup);
  free_transaction_entry(t);

  return FALSE;
}

static gboolean
process_line(struct prog_state *state, const gchar *line, GError **err)
{
  g_assert(state);
  g_assert(line);

  if (g_str_has_prefix(line, CARD_IDSTR_BEGIN_PFX)) {
    if (!handle_card_change(state, line + strlen(CARD_IDSTR_BEGIN_PFX), err)) {
      goto out_fail;
    }
    return TRUE;
  } else if (g_str_has_prefix(line, CARD_IDSTR_END_PFX)) {
    if (!state->curr_card) {
      SET_GERROR(err, -1, "got card end, but no current card!");
      goto out_fail;
    }
    g_message("Closed session for card '%s', %u transactions to date",
              state->curr_card->holder, state->curr_card->transactions->len);
    state->curr_card = NULL;
    return TRUE;
  } else if (is_amex_transaction(line, NULL, NULL)) {
      if (!process_transaction_line(state, line, err)) {
        goto out_fail;
      }
    return TRUE;
  } else if (g_str_has_prefix(line, OCR_IDSTR) && !state->faktura_ocr) {
    state->faktura_ocr = g_strdup(line + strlen(OCR_IDSTR));
  } else if (g_str_has_prefix(line, DUE_DATE_IDSTR)) {
    GError *lerr = NULL;
    gchar *tmp = g_strstrip(g_strdup(line + strlen(DUE_DATE_IDSTR)));

    if ((state->faktura_due_date = parse_amex_date(tmp, &lerr)) == NULL) {
      g_warning("Could not extract due date: %s", GERROR_MSG(lerr));
    }
    g_free(tmp);
    g_clear_error(&lerr);
  }

  /* We don't know what to do with this line */
  g_message("Discarding unsupported line '%s'", line);
  state->stats.skipped_lines++;

  return TRUE;

out_fail:
  g_message("Offending line %u: %s\n", state->idx, line);

  return FALSE;
}

static gboolean
process_transactions(struct prog_state *state, GError **err)
{
  g_assert(state);

  for (state->idx = 0; state->idx < state->lines->len; state->idx++) {
    gchar *line = (gchar *) g_ptr_array_index(state->lines, state->idx);

    if (!process_line(state, line, err)) {
      return FALSE;
    }
    state->stats.total_lines++;
  }

  g_message("Processed %u card(s)..", state->cards->len);
  return TRUE;
}

static void
clear_prog_state(struct prog_state *state)
{
  g_assert(state);
  g_clear_pointer(&state->loc_hash, g_hash_table_destroy);
  g_clear_pointer(&state->faktura_due_date, g_date_time_unref);
  g_free(state->faktura_ocr);

  if (state->lines) {
    g_ptr_array_free(state->lines, TRUE);
  }
  if (state->cards) {
    g_ptr_array_free(state->cards, TRUE);
  }

  memset(state, 0, sizeof(*state));
}

static gboolean
populate_location_hash(const gchar *filename, GHashTable *hash, GError **err)
{
  gchar *buffer = NULL;
  gsize flen = 0;
  gchar **splits = NULL;
  gboolean ret = FALSE;
  guint i;
  guint lc;
  guint added = 0;

  g_assert(filename);
  g_assert(hash);

  if (!g_file_get_contents(filename, &buffer, &flen, err)) {
    return TRUE;
  }

  splits = g_strsplit(buffer, "\n", -1);
  lc = g_strv_length(splits);

  for (i = 0; i < lc; i++) {
    if (g_strstr_len(splits[i], -1, "->")) {
      gchar **ss = g_strsplit(splits[i], "->", 3);
      guint sl = g_strv_length(ss);

      if (sl != 2) {
        SET_GERROR(err, -1, "L%d: Invalid location map entry", i + 1);
        g_strfreev(ss);
        goto out;
      }
      added++;
      g_hash_table_insert(hash, g_strstrip(g_strdup(ss[0])),
                          g_strstrip(g_strdup(ss[1])));
      g_strfreev(ss);
    } else if (g_strstr_len(splits[i], -1, " ")) {
      SET_GERROR(err, -1, "L%d: Invalid location '%s'", i, splits[i]);
      goto out;
    }
    added++;
    g_hash_table_insert(hash, g_strdup(splits[i]), g_strdup(splits[i]));
  }

  g_message("Read %zi bytes, %u lines from '%s' and added %u location entries",
            flen, lc, filename, added);
  ret = TRUE;
  /* fall through */
out:
  g_strfreev(splits);
  g_free(buffer);

  return ret;
}

static void
usage(const gchar *errstr, gint exit_code)
{
  g_printerr("AMEX Faktura simple parser v%s\n", PROG_VERSION);

  if (errstr) {
    g_printerr("\nError: %s\n", errstr);
  }

  g_printerr("\nUsage: %s [options] <input file>\n\n"
             " Options:\n"
             "    --outfile          -o      CSV filename to write to\n"
             "    --location-file    -l      File to populate location hash\n"
             "    --split-width      -s      Line split width (default %u)\n"
             "    --help             -h      Show help options\n\n",
             prog_name, DEFAULT_LINE_SPLIT_WIDTH);

  exit(exit_code);
}

static void
dump_transactions(struct prog_state *state)
{
  guint i;
  gdouble ttotal = 0.00;
  g_assert(state);

  g_print("----------------------------------------------------------------------\n"
          " Total cards: %03d\n"
          "----------------------------------------------------------------------\n",
          state->cards->len);

  for  (i = 0; i < state->cards->len; i++) {
    struct amex_card *c = g_ptr_array_index(state->cards, i);
    gdouble ctotal = 0.00;
    guint j;

    g_print("Card %03d: %s\n", i, print_amex_card(c));
    g_print("-------------------------------------------------------------------------------------------------------------\n");

    if (!c->transactions->len) {
      g_print("No transactions for card\n\n");
      continue;
    }

    for (j = 0; j < c->transactions->len; j++) {
      struct transaction *t = g_ptr_array_index(c->transactions, j);
      gchar *tdate = g_date_time_format(t->date, "%F");
      gchar *pdate = t->process_date ? g_date_time_format(t->date, "%F") :
                                       g_strdup("-");
      gchar *val = g_strdup_printf("%.2f kr", t->value_sek);

      g_print("%-10s %-10s %-40s %-30s %-20s\n",
              tdate, pdate, t->details,
              t->location ? t->location : "Unknown",
              val);
      g_free(tdate);
      g_free(pdate);
      g_free(val);
      ctotal += t->value_sek;
    }
    g_print("=============================================================================================================\n"
            "Total purchases for %s: %.2f SEK\n"
            "=============================================================================================================\n\n",
            print_amex_card(c), ctotal);
    ttotal += ctotal;
  }

  g_print("Total for all cards: %.2f SEK\n", ttotal);
  g_print("   Faktura due date: %s\n",
          state->faktura_due_date ? format_dt(state->faktura_due_date) :
                                    "(unknown)");
  g_print("        Faktura OCR: %s\n\n",
          state->faktura_ocr ? state->faktura_ocr : "(unknown)");

}

#define CSV_HEADER_TMPL "Datum;Bokf"SWE_LOWER_OE"rt;Specifikation;Ort;Valuta;Utl.belopp/moms;Belopp\n"

static gboolean
dump_transactions_to_csv(struct prog_state *state, GError **err)
{
  GString *gs;
  guint i;
  guint tc;
  gboolean ret;

  g_assert(state);
  g_assert(state->opts.outfile);

  gs = g_string_new(NULL);

  for  (i = 0, tc = 0; i < state->cards->len; i++) {
    struct amex_card *c = g_ptr_array_index(state->cards, i);

    guint j;

    g_string_append_printf(gs, "AMEX %s\n%s",
                           print_amex_card(c), CSV_HEADER_TMPL);

    for (j = 0; j < c->transactions->len; j++) {
      struct transaction *t = g_ptr_array_index(c->transactions, j);
      gchar *tdate = g_date_time_format(t->date, "%m-%d");
      gchar *pdate = t->process_date ? g_date_time_format(t->date, "%m-%d") :
                                       g_strdup("");

      /* Datum;Bokfört;Specifikation;Ort;Valuta;Utl.belopp/moms;Belopp */
      g_string_append_printf(gs, "%s;%s;%s;%s;;;%.2f\n",
                             tdate,
                             pdate,
                             t->details,
                             t->location ? t->location : "unknown",
                             t->value_sek);
      g_free(tdate);
      g_free(pdate);
      tc++;
    }
    g_string_append_printf(gs, "\n");
  }

  if ((ret = g_file_set_contents(state->opts.outfile,
                                 gs->str, -1, err)) == FALSE) {
    goto out;
  }

  g_message("Wrote %u transaction(s) to CSV file '%s'",
            tc, state->opts.outfile);

out:
  g_string_free(gs, TRUE);

  return ret;
}

int main(int argc, gchar **argv)
{
  GError *err = NULL;
  struct prog_state state = { 0, };
  struct prog_options *opts = &state.opts;
  gint ret = EXIT_FAILURE;
  gchar *eptr = NULL;
  gint opt;

  static const struct option long_opts[] = {
    { "help",          no_argument,       NULL, 'h' },
    { "outfile",       required_argument, NULL, 'o' },
    { "location-file", required_argument, NULL, 'l' },
    { "split-width",   required_argument, NULL, 's' },
    { NULL,            0,                 NULL,  0  }
  };

  prog_name = argv[0];
  if (argc < 2) {
    usage("Too few arguments", EXIT_FAILURE);
  }

  while ((opt = getopt_long(argc, argv, "hl:o:s:", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'h':
      usage(NULL, EXIT_SUCCESS);
      break;
    case 'o':
      opts->outfile = optarg;
      break;
    case 'l':
      opts->location_file = optarg;
      break;
    case 's':
      opts->line_split_width = g_ascii_strtoll(optarg, &eptr, 10);
      if (opts->line_split_width < MIN_LINE_SPLIT_WIDTH ||
         (eptr && strlen(eptr))) {
        usage("Invalid line split width. Minimum is "
              G_STRINGIFY(MIN_LINE_SPLIT_WIDTH), EXIT_FAILURE);
      }
      break;
    default:
      usage("Illegal option", EXIT_FAILURE);
      break;
    }
  }

  if (optind >= argc) {
    usage("Missing input filename", EXIT_FAILURE);
  }

  if (!opts->line_split_width) {
    opts->line_split_width = DEFAULT_LINE_SPLIT_WIDTH;
  }
  g_message("Using %sline split width of %u",
            opts->line_split_width == DEFAULT_LINE_SPLIT_WIDTH ? "default " : "",
            opts->line_split_width);

  opts->infile = argv[optind++];
  /* Initialise program state */
  state.lines = g_ptr_array_new_with_free_func(g_free);
  state.cards = g_ptr_array_new_with_free_func(free_amex_card);
  state.loc_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, g_free);

  /* Populate the location hash */
  if (opts->location_file && !populate_location_hash(opts->location_file,
                                                     state.loc_hash,
                                                     &err)) {
    g_printerr("Could not parse location file: %s\n", GERROR_MSG(err));
    goto out;
  }

  /* Read the file */
  if (!split_lines_file(opts->infile, opts->line_split_width,
                        state.lines, &err)) {
    g_printerr("Could not parse input file: %s\n", GERROR_MSG(err));
    goto out;
  }

  /* Build the transaction state */
  if (!process_transactions(&state, &err)) {
    g_printerr("Could not process transactions: %s\n", GERROR_MSG(err));
    goto out;
  }

  /* Dump the transactions */
  dump_transactions(&state);

  if (opts->outfile && !dump_transactions_to_csv(&state, &err)) {
    g_printerr("Could not dump to CSV: %s\n", GERROR_MSG(err));
    goto out;
  }

  ret = EXIT_SUCCESS;
  /* fall through */
out:
  g_clear_error(&err);
  clear_prog_state(&state);

  return ret;
}
