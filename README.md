# AMEX Statement Parser

This utility helps parse the truly awful format of the new American Express
PDF statements (bills) into transactions that can be written to a CSV file
and imported into Excel (or your financial program of choice)

## Background
When I first got my Amex Elite card in 2017, the e-statement was nicely laid
out, with one transaction per line and it was easy to copy-paste data from
the PDF into e.g. Excel. Back then Amex offered the option for a physical
(paper) bill, which I have never used.

All was fine and dandy until some time in 2019 when Amex decided that they
would do away with the paper bills altogether - for the environment of course.
While this is in itself commendable, they also changed the format of the
e-statement to a compressed split-column layout. While this greatly reduced
the number of pages (which is apparently super important for PDF documents?!),
it also reduced the level of detail per transaction and made it impossible
to copy-paste individual transactions which has annoyed me immensely.

Despite the year being 2021, and my Amex being the most expensive credit
card I own, they (Amex SE) don't offer the *fundamental* functionality of
exporting transactions to CSV. Yep, you read right, a card that costs
4000SEK+ a year but does not have the possibility to export anything, except
for the above mentioned split-column PDFs.

My SAS Eurobonus Mastercard is a fraction of the price, and not only are
the PDF statements far better laid out, they have a CSV export option.
I could have lived with Amex's horrible PDF bills if they provided a means
to export the transactions. A feature like this appears to be extremely
complicated for them to implement as I have contacted them multiple times about
this gaping hole in their product and have been fobbed off with "we take
your feedback seriously, but cannot provide any timeline". Gah.

After yet another pointless interaction with their customer support, I decided
to deal with the problem myself. I use a spreadsheet to combine our shared
expenses for the month, and prior to this utility I needed to manually plug
in each transaction from the Amex bill .. monkey work. This is what sparked me
to write this parser.

## Functional description
The parser takes a text file representation of the statement and extracts the
transactions from it, separated by cardholder. The total spent for each
cardholder is displayed at the end of the transaction group, as well as the
statement's AVI/OCR number. If provided with an output file option, the
transactions are dumped to CSV (separated by cardholder) with the same column
layout used by the SAS Eurobonus Mastercard.

## Prerequisites
- GLib/GIO
- poppler-utils (For the pdftotext utility)
- Meson (Ninja)

## Building the binary
Clone the repository and after ensuring you have all the pre-reqs run:

```
meson build

ninja -C build
```

## Preparing the statement
AMEX delivers the bills in a "compact" PDF format, split into two columns.
As mentioned, this makes copy-paste of individual transactions impossible,
so we need to convert this atrocity into something our program can handle
and to achieve this we use **pdftotext**, part of the poppler-utils on Debian.
This will convert the PDF into plain text.

```
pdftotext <amex_statement.pdf> -layout <outfile.txt>
```

It is important that the **-layout** option is passed so the column layout
is maintained. Once you have the text file, run the binary. For example:

```
./build/amex-parser <infile.txt> [[-o <outfile.csv>] [-l <locations.txt>] [-s <line split> ]]
```

## Command line options
| Option           | Opt | Description                                             |
| ---------------- |:---:|---------------------------------------------------------|
| --outfile        | -o  | Optional output CSV file. Existing files are ovewritten |
| --location-file  | -l  | Text file containing purchase locations                 |
| --split-width    | -s  | Consider the page split here (default 80 chars)         |
| --help           | -h  | Display command line help                               |


### Location file
The location file is specified with the **-l** option, and is the path to a text
file containing a list of locations separated by a new line. e.g.

```
Arlöv
Stockholm
Malmö
```
In the Amex statement, the location where the transaction was made is not always
in the same place which makes it harder to assume. It is normally the last token
of the line before the amount. So the parser will look at the last token in the
description (which is sometimes on the following line) and try to match this to
an entry in the locations file.

If there is no match found, then the location for that transaction is set to
**Unknown**.

In some (many cases) the location provided by the merchant is misspelt or does
omits the appropriate Swedish characters. The parser handles this by allowing
the locations file to contain a mapping, e.g.:

```
Arloev -> Arlöv
Arlov -> Arlöv
```

If no location file is provided, then all transactions will have an origin of
'Unknown'.

## CSV output format
If a CSV output filename is provided then the transactions will be sorted by
cardholder, with a **;** being the designated separator.
The column headers are the same as the SAS Eurobonus Mastercard export:

*Datum;Bokfört;Specifikation;Ort;Valuta;Utl.belopp/moms;Belopp*

*Valuta* and *Utl.belopp/moms* columns are always empty in the current
implementation.

## Line split width
Normally the split of the two columns on the page is at 80 characters. But of
course, being Amex, this is not consistent between statements. Sometimes it is
90 characters for example. Rather than writing extra code to detect the line
split, it is provided as an option.

### Why is this written in C and not <insert your choice of Go/Python/Rust/Java/Bash>?
Mainly because I like C. Even though it's arguably more code than say, a
Python program, there are many useful libraries available that make programming
in C quite manageable. Not to mention the added advantage of portability
and a small footprint.

## Future improvements
- Handle "inbetalningar"
- Reduce verbose output and/or make it configurable
- Extra

### Disclaimer
I haven not examined other formats of American Express bills; these are the
format that is used in Sweden, YMWV. In fact, this will probably only work for
Amex Sweden customers as the headings etc are in Swedish. You can probably
change these to suit your region (#defines at the top of the amex_parser.c file)

This is in no way a full parser as it does not handle "Inbetalningar". The
output is kept deliberately verbose so I can easily see where parsing goes wrong.
