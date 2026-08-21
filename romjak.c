/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define MAXROMS		16
#define MAXROMWIDTH	32
#define MAXSTRIDE	(MAXROMWIDTH / 8)
#define MAXBANKS	4
#define MAXNAME		256
/* Leaves room for the ".<bank>.<rom>" suffix we append */
#define MAXBASENAME	(MAXNAME - 16)

/*
 * How a binary is spread over a set of ROMs.
 *
 * The ROMs of a bank sit side by side on the data bus, so consecutive
 * bytes of the binary land in different chips: each ROM takes `stride`
 * bytes (its share of the bus width) before the next one gets a turn.
 * Banks stack up the address space, one whole bank after another.
 *
 * Splitting and joining walk exactly the same geometry, one in each
 * direction, which is why they share this.
 */
struct romgeom {
	/* as asked for */
	int numroms;
	int romsize;
	int romwidth;		/* bits */
	int rombanks;
	int paduptosize;

	/* worked out from the above */
	int stride;		/* bytes one ROM takes at a time */
	int romsperbank;
	int banksz;
	int totalsz;
	int repeats;
};

/*
 * Fill in the derived fields and check the whole lot hangs together.
 * Returns NULL if it does, or a message saying what does not.
 */
static const char *geom_derive(struct romgeom *g)
{
	static char err[192];

	if (g->numroms < 1) {
		snprintf(err, sizeof(err), "there must be at least 1 ROM");
		return err;
	}
	if (g->numroms > MAXROMS) {
		snprintf(err, sizeof(err), "sorry, too many ROMs (max %d)", MAXROMS);
		return err;
	}
	if (g->romsize < 1) {
		snprintf(err, sizeof(err), "romsize must be at least 1 byte");
		return err;
	}
	if (g->rombanks < 1) {
		snprintf(err, sizeof(err), "there must be at least 1 bank");
		return err;
	}
	if (g->rombanks > MAXBANKS) {
		snprintf(err, sizeof(err), "sorry, too many banks (max %d)", MAXBANKS);
		return err;
	}
	if ((g->numroms % g->rombanks) != 0) {
		snprintf(err, sizeof(err),
				"number of ROMs (%d) must be a multiple of the number of banks (%d)",
				g->numroms, g->rombanks);
		return err;
	}
	if ((g->romwidth % 8) != 0) {
		snprintf(err, sizeof(err), "ROM width must be a multiple of 8 bits");
		return err;
	}
	if (g->romwidth < 8 || g->romwidth > MAXROMWIDTH) {
		snprintf(err, sizeof(err), "ROM width must be between 8 and %d bits",
				MAXROMWIDTH);
		return err;
	}

	g->stride = g->romwidth / 8;
	g->romsperbank = g->numroms / g->rombanks;
	g->banksz = g->romsize * g->romsperbank;
	g->totalsz = g->romsize * g->numroms;

	if ((g->romsize % g->stride) != 0) {
		snprintf(err, sizeof(err),
				"romsize (%d) must be a multiple of the ROM stride (%d bytes)",
				g->romsize, g->stride);
		return err;
	}

	if (g->paduptosize < 1)
		g->paduptosize = g->totalsz;

	/*
	 * The input is walked sequentially, one stride at a time, and
	 * rewound every time we cross a paduptosize boundary. That only
	 * lines up if the boundaries fall on a stride, and if the output
	 * is a whole number of copies.
	 */
	if ((g->paduptosize % g->stride) != 0) {
		snprintf(err, sizeof(err),
				"paduptosize (%d) must be a multiple of the ROM stride (%d bytes)",
				g->paduptosize, g->stride);
		return err;
	}
	if ((g->totalsz % g->paduptosize) != 0) {
		snprintf(err, sizeof(err),
				"total size (%d) must be a multiple of paduptosize (%d)",
				g->totalsz, g->paduptosize);
		return err;
	}

	g->repeats = g->totalsz / g->paduptosize;

	return NULL;
}

/*
 * Work out a default output basename from the input path:
 * "roms/game.bin" becomes "game".
 */
static void default_basename(const char *path, char *out, size_t outsz)
{
	const char *slash = strrchr(path, '/');
	const char *start = slash ? slash + 1 : path;
	const char *dot = strrchr(start, '.');
	size_t len = dot ? (size_t)(dot - start) : strlen(start);

	/* A name like ".bin" has no stem, so keep the lot */
	if (len == 0)
		len = strlen(start);
	if (len >= outsz)
		len = outsz - 1;
	if (len == 0) {
		snprintf(out, outsz, "rom");
		return;
	}

	memcpy(out, start, len);
	out[len] = '\0';
}

/*
 * Name every image. One bank gets "<base>.<rom>", several banks get
 * "<base>.<bank>.<rom>" so the ordering stays obvious.
 */
static void geom_names(const struct romgeom *g, const char *basename,
		char names[MAXBANKS][MAXROMS][MAXNAME])
{
	for (int i = 0; i < g->rombanks; i++) {
		for (int j = 0; j < g->romsperbank; j++) {
			if (g->rombanks == 1)
				snprintf(names[i][j], MAXNAME, "%s.%d", basename, j);
			else
				snprintf(names[i][j], MAXNAME, "%s.%d.%d", basename, i, j);
		}
	}
}

static int get_arg_or_default(struct arg_int *arg, int defval)
{
	if (arg->count)
		return arg->ival[0];
	else
		return defval;
}

/*
 * Walk the combined address space one stride at a time, in the order the
 * bytes appear in the binary, handing back which bank and ROM each stride
 * belongs to. This is the whole interleave, and both directions use it.
 */
#define for_each_stride(g, bank, rom, pos)					\
	for (int bank = 0; bank < (g)->rombanks; bank++)			\
		for (int row = 0; row < (g)->banksz;				\
				row += (g)->romsperbank * (g)->stride)		\
			for (int rom = 0, pos = (bank) * (g)->banksz + row;	\
					rom < (g)->romsperbank;			\
					rom++, pos += (g)->stride)

static int cmd_split(int argc, char **argv)
{
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
			"How much to pad the input data up to. "
			"For example if you have a 4KB input, "
			"pad up to 32KB and the total is 64KB "
			"you'll get two copies of the input "
			"padded up to 32KB with 0xff. "
			"If the input is bigger than this value "
			"it will be truncated. "
			"If this value is missing padding will be "
			"added to fill up the total size.";

	static const char *basename_help =
			"Base name for the outputs, defaults to something based on the input path";

	void *argtable[] = {
		help            = arg_litn("h", "help", 0, 1, "display this help and exit"),
		arg_numroms     = arg_int1(NULL, "numroms", "<n>", "Total number of ROMs"),
		arg_romwidth    = arg_intn(NULL, "romwidth", "<n>", 0, 1, romwidth_help),
		arg_romsize     = arg_int1(NULL, "romsize", "<n>", "Size of a single ROM in bytes"),
		arg_rombanks    = arg_intn(NULL, "rombanks", "<n>", 0, 1, "How many banks of ROMs, defaults to 1"),
		arg_paduptosize = arg_intn(NULL, "paduptosize", "<n>", 0, 1, paduptosize_help),
		arg_input       = arg_file1(NULL, NULL, "<file>", "input file"),
		arg_basename    = arg_strn(NULL, NULL, "<output basename>", 0, 1, basename_help),
		end             = arg_end(20),
	};

	const char *progname = "romjak split";
	int nerrors = arg_parse(argc, argv, argtable);

	if (help->count > 0) {
		printf("Usage: %s", progname);
		arg_print_syntax(stdout, argtable, "\n");
		printf("Split a binary across a set of ROM images for burning.\n\n");
		arg_print_glossary(stdout, argtable, "  %-25s %s\n");
		printf("\nSizes accept 0x hex and KB/MB suffixes, eg --romsize=32KB\n");
		arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
		return 0;
	}

	if (nerrors > 0) {
		arg_print_errors(stderr, end, progname);
		fprintf(stderr, "Try 'romjak split --help' for more information.\n");
		arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
		return 1;
	}

	struct romgeom g = {
		.numroms     = arg_numroms->ival[0],
		.romsize     = arg_romsize->ival[0],
		.romwidth    = get_arg_or_default(arg_romwidth, 8),
		.rombanks    = get_arg_or_default(arg_rombanks, 1),
		.paduptosize = get_arg_or_default(arg_paduptosize, 0),
	};

	const char *err = geom_derive(&g);
	if (err) {
		fprintf(stderr, "%s: %s\n", progname, err);
		return 1;
	}

	/* Work out the resulting file names */
	char basename[MAXBASENAME];
	if (arg_basename->count && arg_basename->sval[0] && arg_basename->sval[0][0])
		snprintf(basename, sizeof(basename), "%s", arg_basename->sval[0]);
	else
		default_basename(arg_input->filename[0], basename, sizeof(basename));

	char names[MAXBANKS][MAXROMS][MAXNAME] = { 0 };
	geom_names(&g, basename, names);

	/* Print it all out because I no good at math */
	printf("Going to create outputs for %d ROMs:\n"
		   " - Total data to generate %d bytes, %d bytes per bank\n"
		   " - Each image will be %d bytes long\n"
		   " - Input data stride (how many bytes put into an output at a time) is %d bytes\n"
		   " - Input data will be repeated %d times\n",
			g.numroms, g.totalsz, g.banksz, g.romsize, g.stride, g.repeats);

	printf("Your output images will be like this:\n");
	for (int i = 0; i < g.rombanks; i++) {
		unsigned int bank_start = g.banksz * i;
		unsigned int bank_end = (g.banksz * (i + 1)) - 1;

		printf(" - bank %d [0x%08x - 0x%08x]:", i, bank_start, bank_end);
		for (int j = 0; j < g.romsperbank; j++)
			printf(" rom %d - %s", j, names[i][j]);
		printf("\n");
	}

	/* Open all of the files */
	FILE *outputs[MAXBANKS][MAXROMS] = { 0 };
	FILE *input = fopen(arg_input->filename[0], "rb");
	if (!input) {
		fprintf(stderr, "Couldn't open the input file '%s': %s\n",
				arg_input->filename[0], strerror(errno));
		return 1;
	}

	/* Get the input size */
	fseek(input, 0L, SEEK_END);
	long inputsize = ftell(input);
	rewind(input);

	if (inputsize <= 0) {
		fprintf(stderr, "The input file is empty\n");
		return 1;
	}

	for (int i = 0; i < g.rombanks; i++) {
		for (int j = 0; j < g.romsperbank; j++) {
			outputs[i][j] = fopen(names[i][j], "wb");
			if (!outputs[i][j]) {
				fprintf(stderr, "Couldn't open '%s' for writing: %s\n",
						names[i][j], strerror(errno));
				return 1;
			}
		}
	}

	printf("Doing it..\n");

	for_each_stride(&g, this_bank, this_rom, pos_abs) {
		FILE *output = outputs[this_bank][this_rom];
		uint8_t data[MAXSTRIDE];
		memset(data, 0xff, sizeof(data));

		/* Where are we in the current repeat of the input? */
		unsigned int pos_repeat = pos_abs % g.paduptosize;

		if (pos_repeat == 0)
			rewind(input);

		/*
		 * A short read at the tail of the input just leaves the rest
		 * of the stride as pad, which is what we want.
		 */
		if (pos_repeat < (unsigned long)inputsize)
			fread(data, 1, g.stride, input);

		if (fwrite(data, 1, g.stride, output) != (size_t)g.stride) {
			fprintf(stderr, "Write to '%s' failed: %s\n",
					names[this_bank][this_rom], strerror(errno));
			return 1;
		}
	}

	if (ferror(input)) {
		fprintf(stderr, "Read from '%s' failed: %s\n",
				arg_input->filename[0], strerror(errno));
		return 1;
	}

	fclose(input);
	for (int i = 0; i < g.rombanks; i++) {
		for (int j = 0; j < g.romsperbank; j++) {
			if (fclose(outputs[i][j]) != 0) {
				fprintf(stderr, "Couldn't finish writing '%s': %s\n",
						names[i][j], strerror(errno));
				return 1;
			}
		}
	}

	printf("Done\n");

	arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));

	return 0;
}

static int cmd_join(int argc, char **argv)
{
	struct arg_lit *help;
	struct arg_int *arg_romwidth, *arg_rombanks, *arg_trim;
	struct arg_file *arg_output, *arg_roms;
	struct arg_end *end;

	static const char *roms_help =
			"ROM images, in bus order within a bank and then bank by bank, "
			"ie the same order 'romjak split' wrote them";

	static const char *trim_help =
			"Truncate the output to this many bytes, to drop the padding "
			"a split added";

	void *argtable[] = {
		help         = arg_litn("h", "help", 0, 1, "display this help and exit"),
		arg_romwidth = arg_intn(NULL, "romwidth", "<n>", 0, 1,
					"Data bus width of a single ROM in bits (multiple of 8), defaults to 8"),
		arg_rombanks = arg_intn(NULL, "rombanks", "<n>", 0, 1,
					"How many banks the ROMs are split into, defaults to 1"),
		arg_trim     = arg_intn(NULL, "trim", "<n>", 0, 1, trim_help),
		arg_output   = arg_file1("o", "output", "<file>", "where to write the joined binary"),
		arg_roms     = arg_filen(NULL, NULL, "<rom>", 1, MAXROMS, roms_help),
		end          = arg_end(20),
	};

	const char *progname = "romjak join";
	int nerrors = arg_parse(argc, argv, argtable);

	if (help->count > 0) {
		printf("Usage: %s", progname);
		arg_print_syntax(stdout, argtable, "\n");
		printf("Join a set of ROM images back into the binary they came from.\n\n");
		arg_print_glossary(stdout, argtable, "  %-25s %s\n");
		printf("\nSizes accept 0x hex and KB/MB suffixes, eg --trim=32KB\n");
		arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
		return 0;
	}

	if (nerrors > 0) {
		arg_print_errors(stderr, end, progname);
		fprintf(stderr, "Try 'romjak join --help' for more information.\n");
		arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
		return 1;
	}

	/*
	 * Unlike split, the geometry is mostly read off the files we were
	 * given: one ROM per argument, all the same size.
	 */
	FILE *inputs[MAXROMS] = { 0 };
	long romsize = 0;

	for (int i = 0; i < arg_roms->count; i++) {
		const char *name = arg_roms->filename[i];

		inputs[i] = fopen(name, "rb");
		if (!inputs[i]) {
			fprintf(stderr, "Couldn't open '%s': %s\n", name, strerror(errno));
			return 1;
		}

		fseek(inputs[i], 0L, SEEK_END);
		long sz = ftell(inputs[i]);
		rewind(inputs[i]);

		if (sz <= 0) {
			fprintf(stderr, "'%s' is empty\n", name);
			return 1;
		}
		if (i == 0) {
			romsize = sz;
		} else if (sz != romsize) {
			fprintf(stderr,
					"'%s' is %ld bytes but '%s' is %ld - all the ROMs must be the same size\n",
					name, sz, arg_roms->filename[0], romsize);
			return 1;
		}
	}

	struct romgeom g = {
		.numroms  = arg_roms->count,
		.romsize  = (int)romsize,
		.romwidth = get_arg_or_default(arg_romwidth, 8),
		.rombanks = get_arg_or_default(arg_rombanks, 1),
	};

	const char *err = geom_derive(&g);
	if (err) {
		fprintf(stderr, "%s: %s\n", progname, err);
		return 1;
	}

	int trim = get_arg_or_default(arg_trim, 0);
	if (trim < 0 || trim > g.totalsz) {
		fprintf(stderr, "trim (%d) must be between 0 and the joined size (%d)\n",
				trim, g.totalsz);
		return 1;
	}

	printf("Going to join %d ROMs of %d bytes into %s:\n"
		   " - Total data %d bytes, %d bytes per bank across %d bank(s)\n"
		   " - Output data stride (how many bytes taken from a ROM at a time) is %d bytes\n",
			g.numroms, g.romsize, arg_output->filename[0],
			g.totalsz, g.banksz, g.rombanks, g.stride);

	printf("Reading the ROMs in this order:\n");
	for (int i = 0; i < g.rombanks; i++) {
		unsigned int bank_start = g.banksz * i;
		unsigned int bank_end = (g.banksz * (i + 1)) - 1;

		printf(" - bank %d [0x%08x - 0x%08x]:", i, bank_start, bank_end);
		for (int j = 0; j < g.romsperbank; j++)
			printf(" rom %d - %s", j, arg_roms->filename[i * g.romsperbank + j]);
		printf("\n");
	}

	FILE *output = fopen(arg_output->filename[0], "wb");
	if (!output) {
		fprintf(stderr, "Couldn't open '%s' for writing: %s\n",
				arg_output->filename[0], strerror(errno));
		return 1;
	}

	printf("Doing it..\n");

	/*
	 * Count the run of pad bytes at the tail so we can tell the user how
	 * much of what they just got back is filler rather than data.
	 */
	long written = 0;
	long trailing_pad = 0;

	for_each_stride(&g, this_bank, this_rom, pos_abs) {
		FILE *in = inputs[this_bank * g.romsperbank + this_rom];
		uint8_t data[MAXSTRIDE];

		if (fread(data, 1, g.stride, in) != (size_t)g.stride) {
			fprintf(stderr, "Read from '%s' failed: %s\n",
					arg_roms->filename[this_bank * g.romsperbank + this_rom],
					ferror(in) ? strerror(errno) : "unexpected end of file");
			return 1;
		}

		int n = g.stride;
		if (trim && written + n > trim)
			n = (int)(trim - written);
		if (n <= 0)
			continue;

		if (fwrite(data, 1, n, output) != (size_t)n) {
			fprintf(stderr, "Write to '%s' failed: %s\n",
					arg_output->filename[0], strerror(errno));
			return 1;
		}

		for (int k = 0; k < n; k++) {
			if (data[k] == 0xff)
				trailing_pad++;
			else
				trailing_pad = 0;
		}

		written += n;
	}

	for (int i = 0; i < g.numroms; i++)
		fclose(inputs[i]);

	if (fclose(output) != 0) {
		fprintf(stderr, "Couldn't finish writing '%s': %s\n",
				arg_output->filename[0], strerror(errno));
		return 1;
	}

	printf("Wrote %ld bytes to %s\n", written, arg_output->filename[0]);
	/* A byte or two of 0xff at the end is just data, don't cry wolf */
	if (trailing_pad >= 16)
		printf("Note: the last %ld bytes are 0xff, so they are probably padding.\n"
			   "      Use --trim=<n> to cut the output down to the real data.\n",
				trailing_pad);

	printf("Done\n");

	arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));

	return 0;
}

static void usage(FILE *out)
{
	fprintf(out,
		"romjak - for jacking them roms\n"
		"\n"
		"Usage: romjak <command> [options]\n"
		"\n"
		"Commands:\n"
		"  split    Split a binary across a set of ROM images for burning\n"
		"  join     Join a set of ROM images back into one binary\n"
		"\n"
		"Try 'romjak <command> --help' for the options of each.\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 1;
	}

	if (!strcmp(argv[1], "split"))
		return cmd_split(argc - 1, argv + 1);

	if (!strcmp(argv[1], "join"))
		return cmd_join(argc - 1, argv + 1);

	if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
		usage(stdout);
		return 0;
	}

	fprintf(stderr, "romjak: unknown command '%s'\n\n", argv[1]);
	usage(stderr);
	return 1;
}
