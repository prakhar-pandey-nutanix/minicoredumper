/*
 * Copyright (c) 2012-2018 Linutronix GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "common.h"

/*
 * This program injects binary data dumped by the minicoredumper into a
 * core file. The required files generated by the minicoredumper are:
 *   - core file
 *   - symbol.map
 *   - binary dump files (and/or --data specified direct data)
 */

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <options> <core> <symbol.map> [binary-dump]...\n",
		argv0);
	fprintf(stderr, "\n");
	fprintf(stderr, "Available options:\n");
	fprintf(stderr, "  --data=<ident>:<bytecount>@<source-file>+<source-offset>\n");
	fprintf(stderr, "        Inject <bytecount> bytes of data at offset <source-offset>\n");
	fprintf(stderr, "        of file <source-file> to the core. The data is injected to\n");
	fprintf(stderr, "        to the position of the <ident> stored in the symbol map.\n");
}

struct ident_data {
	const char *filename;
	const char *ident;
	unsigned long dump_offset;
	unsigned long core_offset;
	unsigned long mem_offset;
	unsigned long size;
};

struct prog_option {
	int processed;

	size_t size;
	size_t offset;
	char *ident;
	char *filename;

	struct prog_option *next;
};

static struct core_data *dump_list;

static void add_dump_item(unsigned long mem_offset, unsigned long size)
{
	struct core_data *cd;

	cd = calloc(1, sizeof(*cd));
	if (!cd)
		return;

	cd->mem_start = mem_offset;
	cd->start = 0;
	cd->end = size;
	cd->next = dump_list;

	dump_list = cd;
}

static int write_core(FILE *f_core, FILE *f_dump, struct ident_data *d,
		      int direct)
{
	char *buf = NULL;
	int err = -1;

	/* seek in core */
	if (fseek(f_core, d->core_offset, SEEK_SET) != 0) {
		fprintf(stderr, "error: failed to seek to position 0x%lx for "
				"ident %s in core (%s)\n",
			d->core_offset, d->ident, strerror(errno));
		goto out;
	}

	/* seek in dump */
	if (fseek(f_dump, d->dump_offset, SEEK_SET) != 0) {
		fprintf(stderr, "error: failed to seek to position 0x%lx for "
				"ident %s in dump (%s)\n",
			d->dump_offset, d->ident, strerror(errno));
		goto out;
	}

	/* alloc data buffer */
	buf = malloc(d->size);
	if (!buf) {
		fprintf(stderr, "error: out of memory allocating %ld bytes\n",
			d->size);
		goto out;
	}

	/* read from dump */
	if (fread(buf, d->size, 1, f_dump) != 1) {
		fprintf(stderr, "error: failed to read %ld bytes from dump\n",
			d->size);
		if (direct) {
			fprintf(stderr, "  specify the data source for %s with:\n",
				d->ident);
			fprintf(stderr, "  --data=%s:%ld@<filename>+<offset>\n",
				d->ident, d->size);
		}
		goto out;
	}

	/* write to core */
	if (fwrite(buf, d->size, 1, f_core) != 1) {
		fprintf(stderr, "error: failed to write %ld bytes to "
				"core (%s)\n", d->size, strerror(errno));
		goto out;
	}

	add_dump_item(d->mem_offset, d->size);

	printf("injected: %s, %ld bytes, %s\n", d->ident, d->size,
	       direct ? "direct" : "indirect");

	err = 0;
out:
	if (buf)
		free(buf);

	return err;
}

static int get_ident_data(const char *ident, FILE *f_symmap,
			  struct ident_data *direct,
			  struct ident_data *indirect)
{
	struct ident_data *d;
	unsigned long mem;
	off64_t offset;
	char line[128];
	size_t size;
	char type;
	char *p;
	int i;

	memset(direct, 0, sizeof(*direct));
	memset(indirect, 0, sizeof(*indirect));

	/* Search the full symbol map to find the ident information for
	 * the specified ident. If the number of idents in a symbol map
	 * become large and the number of dump files becomes large, then
	 * it would be more efficient to parse the system map once,
	 * allocating ident information along the way. */

	rewind(f_symmap);

	while (fgets(line, sizeof(line), f_symmap)) {
		/* strip newline */
		p = strchr(line, '\n');
		if (p)
			*p = 0;

		/* ignore invalid lines */
		if (sscanf(line, "%" PRIx64 " %lx %zx %c ", &offset, &mem,
			   &size, &type) != 4) {
			continue;
		}

		/* locate ident name */
		p = line;
		for (i = 0; i < 4; i++) {
			p = strchr(p, ' ');
			if (!p)
				break;
			p++;
		}
		/* ignore invalid lines */
		if (i != 4)
			continue;

		/* check if this is the ident we want */
		if (strcmp(ident, p) != 0)
			continue;

		if (type == 'D' || type == 'N') {
			d = direct;
		} else if (type == 'I') {
			d = indirect;
		} else {
			/* ignore invalid lines */
			continue;
		}

		/* last entry wins in case of duplicates */
		d->core_offset = offset;
		d->mem_offset = mem;
		d->size = size;
		d->ident = ident;
	}

	/* If indirect data exists, the direct data will come after it in
	 * the dump file. Adjust the direct data dump offset accordingly. */
	if (indirect->size && direct->size)
		direct->dump_offset += indirect->size;

	return 0;
}

static void check_user_data(struct ident_data *d, struct prog_option *options)
{
	struct prog_option *o;

	for (o = options; o; o = o->next) {
		if (o->processed)
			continue;

		if (strcmp(o->ident, d->ident) == 0) {
			d->size = o->size;
			d->dump_offset = o->offset;
			d->filename = o->filename;
			o->processed = 1;
		}
	}
}

static int inject_data(FILE *f_core, FILE *f_symmap, const char *b_fname,
		       struct prog_option *options)
{
	struct ident_data indirect;
	struct ident_data direct;
	const char *ident;
	FILE *f_dump;
	int err = 0;
	char *p;

	/* extract ident name from file path */
	p = strrchr(b_fname, '/');
	if (p)
		ident = p + 1;
	else
		ident = b_fname;

	/* get offsets/sizes from symbol map */
	if (get_ident_data(ident, f_symmap, &direct, &indirect) != 0) {
		fprintf(stderr, "error: unable to find ident %s in map\n",
			ident);
		return -1;
	}

	if (direct.size > 0) {
		direct.filename = b_fname;

		/* replace/insert any user specified direct data */
		check_user_data(&direct, options);

		/* open binary dump file for reading */
		f_dump = fopen(direct.filename, "r");
		if (!f_dump) {
			fprintf(stderr, "error: failed to open %s (%s)\n",
				direct.filename, strerror(errno));
			return -1;
		}

		/* write direct data (continuing on error) */
		err |= write_core(f_core, f_dump, &direct, 1);

		fclose(f_dump);
	}

	if (indirect.size > 0) {
		indirect.filename = b_fname;

		/* open binary dump file for reading */
		f_dump = fopen(indirect.filename, "r");
		if (!f_dump) {
			fprintf(stderr, "error: failed to open %s (%s)\n",
				indirect.filename, strerror(errno));
			return -1;
		}

		/* write indirect data (continuing on error) */
		err |= write_core(f_core, f_dump, &indirect, 0);

		fclose(f_dump);
	}

	return err;
}

static void free_options(struct prog_option *options)
{
	struct prog_option *o;

	while (options) {
		o = options;
		options = o->next;

		if (o->ident)
			free(o->ident);
		if (o->filename)
			free(o->filename);
		free(o);
	}
}

static int add_option(struct prog_option **options, const char *arg)
{
	struct prog_option *o;
	struct prog_option *i;
	const char *p1;
	const char *p2;

	/* only --data= is supported */

	if (strncmp(arg, "--data=", strlen("--data=")) != 0) {
		fprintf(stderr, "error: unknown option: %s\n", arg);
		return -1;
	}

	o = calloc(1, sizeof(*o));
	if (!o) {
		fprintf(stderr, "error: out of memory\n");
		goto err_out;
	}

	p1 = arg + strlen("--data=");
	p2 = strchr(p1, ':');
	if (!p2) {
		fprintf(stderr, "error: invalid --data syntax: %s\n", arg);
		goto err_out;
	}

	o->ident = strndup(p1, p2 - p1);
	if (!o->ident) {
		fprintf(stderr, "error: out of memory\n");
		goto err_out;
	}

	p1 = p2 + 1;
	p2 = strchr(p1, '@');
	if (!p2) {
		fprintf(stderr, "error: invalid --data syntax: %s\n", arg);
		goto err_out;
	}

	if (sscanf(p1, "%zd@", &o->size) != 1) {
		fprintf(stderr, "error: invalid --data syntax: %s\n", arg);
		goto err_out;
	}

	p1 = p2 + 1;
	p2 = strchr(p1, '+');
	if (!p2) {
		fprintf(stderr, "error: invalid --data syntax: %s\n", arg);
		goto err_out;
	}

	o->filename = strndup(p1, p2 - p1);
	if (!o->filename) {
		fprintf(stderr, "error: out of memory\n");
		goto err_out;
	}

	p1 = p2 + 1;
	if (sscanf(p1, "%zd", &o->offset) != 1) {
		fprintf(stderr, "error: invalid --data syntax: %s\n", arg);
		goto err_out;
	}

	/* append to existing options */
	if (!*options) {
		*options = o;
	} else {
		for (i = *options; i->next; i = i->next)
			/* NOP */ ;
		i->next = o;
	}

	return 0;
err_out:
	free_options(o);
	return -1;
}

int main(int argc, char *argv[])
{
	struct prog_option *options = NULL;
	const char *core_filename;
	struct prog_option *o;
	FILE *f_symmap = NULL;
	FILE *f_core = NULL;
	struct stat s;
	int err = 1;
	int i;

	if (argc < 4) {
		usage(argv[0]);
		goto out;
	}

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-')
			break;

		if (add_option(&options, argv[i]) != 0)
			goto out;
	}

	if (i == argc) {
		usage(argv[0]);
		goto out;
	}

	/* check if the core file is present */
	if (stat(argv[i], &s) != 0) {
		fprintf(stderr, "error: failed to stat %s (%s)\n",
			argv[i], strerror(errno));
		goto out;
	}

	/* open the core file read-write */
	core_filename = argv[i];
	f_core = fopen(core_filename, "r+");
	if (!f_core) {
		fprintf(stderr, "error: failed to open %s for writing (%s)\n",
			argv[i], strerror(errno));
		goto out;
	}

	i++;
	if (i == argc) {
		usage(argv[0]);
		goto out;
	}

	/* open the symbol map for reading */
	f_symmap = fopen(argv[i], "r");
	if (!f_symmap) {
		fprintf(stderr, "error: failed to open %s (%s)\n", argv[i],
			strerror(errno));
		goto out;
	}

	err = 0;

	/* try to add binary dumps (continuing on error) */
	for ( ; i < argc; i++) {
		if (inject_data(f_core, f_symmap, argv[i], options) != 0)
			err |= 1;
	}

	/* try to add leftover specified direct data */
	for (o = options; o; o = o->next) {
		if (o->processed)
			continue;

		if (inject_data(f_core, f_symmap, o->ident, options) != 0)
			err |= 1;
	}
out:
	if (f_core)
		fclose(f_core);
	if (f_symmap)
		fclose(f_symmap);
	free_options(options);

	if (err == 0) {
		struct stat sb;
		size_t size;
		int fd;

		fd = open(core_filename, O_RDWR);
		if (fd >= 0) {
			if (fstat(fd, &sb) == 0) {
				size = sb.st_size;
				add_dump_list(fd, &size, dump_list, NULL);
			}
			close(fd);
		}
	}

	while (dump_list) {
		struct core_data *cd = dump_list;
		dump_list = cd->next;
		free(cd);
	}

	return err;
}
