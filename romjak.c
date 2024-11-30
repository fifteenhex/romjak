/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define MAXROMS		16
#define MAXROMWIDTH	32
#define MAXSTRIDE	(MAXROMWIDTH/8)
#define MAXBANKS	4
#define MAXROMSPERBANK (MAXROMS/MAXBANKS)

static int get_arg_or_default(struct arg_int *arg, int defval)
{
	if (arg->count)
		return arg->ival[0];
	else
		return defval;
}

int main(int argc, char **argv) {
	struct arg_lit *help;
	struct arg_int *arg_numroms, *arg_romwidth,
				   *arg_romsize, *arg_rombanks,
				   *arg_paduptosize;
	struct arg_file *arg_input;
	struct arg_str *arg_basename;
	struct arg_end *end;

	static const char *romwidth_help =
			"Data bus width of a single ROM in bits (multiple of 8), defaults to 8";

	static const char *paduptosize_help =
			"How much to pad the input data up to."
			"For example if you have a 4KB input, "
			"pad up to 32KB and the bank is 64KB "
			"you'll get two copies of the input "
			"padded up to 32KB with 0xff."
			"If the input is bigger than this value "
			"it will be truncated."
			"If this value is missing padding will be"
			"added to fill up the total size.";

	static const char *basename_help =
			"Base name for the outputs, defaults to something based on the input path";

	void *argtable[] = {
		help            = arg_litn(NULL, "help", 0, 1, "display this help and exit"),
		arg_numroms     = arg_int1(NULL, "numroms", "<n>", "Total number of ROMs"),
		arg_romwidth    = arg_intn(NULL, "romwidth", "<n>", 0, 1, romwidth_help),
		arg_romsize     = arg_int1(NULL, "romsize", "<n>", "Size of a single ROM in bytes"),
		arg_rombanks    = arg_intn(NULL, "rombanks", "<n>", 0, 1, "How many banks of ROMS, defaults to 1"),
		arg_paduptosize = arg_intn(NULL, "paduptosize", "<n>", 0, 1, paduptosize_help),
		arg_input       = arg_file1(NULL, NULL, "<file>", "input file"),
		arg_basename    = arg_strn(NULL, NULL, "<output basename>", 0, 1, basename_help),
		end             = arg_end(20),
	};

	int nerrors;
	nerrors = arg_parse(argc,argv,argtable);

	char progname[] = "romjak";

	if (help->count > 0)
	{
		printf("Usage: %s", progname);
		arg_print_syntax(stdout, argtable, "\n");
		printf("Demonstrate command-line parsing in argtable3.\n\n");
		arg_print_glossary(stdout, argtable, "  %-25s %s\n");
		exit(0);
	}

	if (nerrors > 0)
	{
		arg_print_errors(stdout, end, progname);
		printf("Try '%s --help' for more information.\n", progname);
		exit(1);
	}

	/*
	 * Get the number of ROMs and size of each
	 * and work out what the total size is.
	 */
	int numroms = *arg_numroms->ival;
	int romsize = *arg_romsize->ival;
	int totalsz = romsize * numroms;

	/*
	 * Get the user specified padding up to size,
	 * set to the total size if unspecified.
	 */
	const int paduptosize = get_arg_or_default(arg_paduptosize, totalsz);

	/* Get the number of ROM banks, 1 if unspecified */
	const int rombanks = get_arg_or_default(arg_rombanks, 1);

	/* Get the width of each ROM, 8 if unspecified */
	const int romwidth = get_arg_or_default(arg_romwidth, 8);

	/* Check the numbers are logical */
	/* Banks */
	if (rombanks > MAXBANKS) {
		printf("Sorry, too many banks\n");
		exit(1);
	}

	/* ROM width */
	if ((romwidth % 8) != 0) {
		printf("ROM width needs to be a multiple of 8\n");
		exit(1);
	}
	if (romwidth > MAXROMWIDTH) {
		printf("ROM width is too big\n");
		exit(1);
	}

#if 0
	/* Pad size */
	if ((romsize % paduptosize) != 0) {
		printf("romsize must be a multiple of paduptosize\n");
		exit(1);
	}
#endif

	/* Number of ROMs */
	if ((numroms % rombanks) != 0) {
		printf("number of ROMs must be a multiple of number of banks\n");
		exit(1);
	}

	if (numroms > MAXROMS) {
		printf("Sorry, too many ROMs\n");
		exit(1);
	}

	/* Calculate the stride, repeats etc, sizes */
	int romsperbank = numroms / rombanks;
	int banksz = romsize * romsperbank;

	int stride = romwidth / 8;
	int repeats = romsize / paduptosize;

	/* Print it all out because I no good at math */
	printf("Going to create outputs for %d ROMs:\n"
		   " - Total data to generate %d bytes, %d bytes per bank\n"
		   " - Each image will be %d bytes long\n"
		   " - Input data stride (how many bytes put into an output at a time) is %d bytes\n"
		   " - Input data will be repeated %d times\n",
			numroms, totalsz, banksz, romsize, stride, repeats);

	/* Work out the resulting file names */
	const char *basename = arg_basename->sval[0];
	char output_names[MAXBANKS][MAXROMS][256] = { 0 };
	if (rombanks == 1) {
		for (int i = 0; i < romsperbank; i++)
			sprintf(output_names[0][i], "%s.%d", basename, i);
	}
	else {
		for (int i = 0; i < rombanks; i++) {
			for (int j = 0; j < romsperbank; j++)
				sprintf(output_names[i][j], "%s.%d.%d", basename, i, j);
		}
	}

	printf("Your output images will be like this:\n");
	for (int i = 0; i < rombanks; i++) {
		unsigned int bank_start = banksz * i;
		unsigned int bank_end = (banksz * (i + 1)) - 1;

		printf(" - bank %d [0x%08x - 0x%08x]:", i, bank_start, bank_end);
		for (int j = 0; j < romsperbank; j++) {
			printf(" rom %d - %s", j, output_names[i][j]);
		}
		printf("\n");
	}

	/* Open all of the files */
	FILE *outputs[MAXBANKS][MAXROMSPERBANK] = { 0 };
	FILE *input = fopen(arg_input->filename[0], "r");
	if (!input) {
		printf("Couldn't open the input file: %d\n", errno);
		exit(1);
	}

	/* Get the input size */
	fseek(input, 0L, SEEK_END);
	int inputsize = ftell(input);
	rewind(input);

	for (int i = 0; i < rombanks; i++) {
		for (int j = 0; j < romsperbank; j++) {
			outputs[i][j] = fopen(output_names[i][j], "w");
			if (!outputs[i][j]) {
				printf("Couldn't open one of the outputs for writing: %d\n", errno);
				exit(1);
			}
		}
	}

	printf("Doing it..\n");

	/* Bank by bank */
	for (int this_bank = 0; this_bank < rombanks; this_bank++) {
		/* Row by row within a bank */
		for (int pos_bank = 0; pos_bank < banksz; pos_bank += (romsperbank * stride))
			/* ROM by ROM with the row */
			for (int this_rom = 0; this_rom < romsperbank; this_rom++) {
				FILE *output = outputs[this_bank][this_rom];
				uint8_t data[MAXSTRIDE];
				memset(data, 0xff, sizeof(data));

				/* Ok, so were are we in the complete output? */
				unsigned int pos_abs = (this_bank * banksz) + pos_bank + (this_rom * stride);
				/* Ok, were are we in the current repeat */
				unsigned int pos_repeat = pos_abs % paduptosize;

				if (pos_repeat == 0)
					rewind(input);

				if (pos_repeat < inputsize)
					fread(data, stride, 1, input);

#if 0
				printf("bank %d, rom %d, bank pos 0x%08x, abs pos 0x%08x, repeat pos 0x%08x\n",
						this_bank, this_rom, pos_bank + (this_rom * stride),
						pos_abs, pos_repeat);
#endif

				fwrite(data, stride, 1, output);
			}
	}

	printf("Done\n");

	return 0;
}
