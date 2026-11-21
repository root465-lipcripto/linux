// SPDX-License-Identifier: GPL-2.0-only
/*
 * Adaptive, userspace-controlled ZRAM recompression experiment.
 *
 * This program is deliberately conservative: it is dry-run by default,
 * requires explicit opt-in to write sysfs, and bounds each recompression
 * batch.  It does not configure ZRAM or select compression algorithms.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SYSFS_ROOT "/sys"
#define DEFAULT_PROC_ROOT "/proc"
#define DEFAULT_DEVICE "zram0"
#define DEFAULT_INTERVAL 10
#define DEFAULT_COOLDOWN 60
#define DEFAULT_MEMORY_PSI 10.0
#define DEFAULT_CPU_PSI 5.0
#define DEFAULT_MIN_USED_MIB 128
#define DEFAULT_PRIORITY 1
#define DEFAULT_MAX_PAGES 256
#define LINE_SIZE 4096

struct options {
	const char *sysfs_root;
	const char *proc_root;
	const char *device;
	unsigned int interval;
	unsigned int cooldown;
	unsigned int max_pages;
	unsigned int priority;
	unsigned long long min_used_bytes;
	double memory_psi_limit;
	double cpu_psi_limit;
	bool apply;
	bool once;
};

static void usage(FILE *out, const char *program)
{
	fprintf(out,
		"Usage: %s [options]\n"
		"  --device NAME             ZRAM device (default: zram0)\n"
		"  --memory-psi PERCENT      Trigger at memory PSI some avg10 (default: %.1f)\n"
		"  --cpu-psi PERCENT         Pause above CPU PSI some avg10 (default: %.1f)\n"
		"  --min-used-mib MIB        Minimum zram mem_used_total (default: %u)\n"
		"  --max-pages COUNT         Maximum pages per batch (default: %u)\n"
		"  --priority NUMBER         Configured recompression priority (default: %u)\n"
		"  --interval SECONDS        Poll interval (default: %u)\n"
		"  --cooldown SECONDS        Minimum delay between batches (default: %u)\n"
		"  --sysfs-root PATH         Sysfs root (default: %s)\n"
		"  --proc-root PATH          Procfs root (default: %s)\n"
		"  --apply                   Allow writes to zram sysfs (default: dry-run)\n"
		"  --once                    Evaluate one sample and exit\n"
		"  --help                    Show this help\n",
		program, DEFAULT_MEMORY_PSI, DEFAULT_CPU_PSI,
		DEFAULT_MIN_USED_MIB, DEFAULT_MAX_PAGES, DEFAULT_PRIORITY,
		DEFAULT_INTERVAL, DEFAULT_COOLDOWN,
		DEFAULT_SYSFS_ROOT, DEFAULT_PROC_ROOT);
}

static bool parse_uint(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || end == text || *end || parsed > UINT_MAX)
		return false;
	*value = (unsigned int)parsed;
	return true;
}

static bool parse_double(const char *text, double *value)
{
	char *end;

	errno = 0;
	*value = strtod(text, &end);
	return !errno && end != text && !*end && isfinite(*value) &&
		*value >= 0.0 && *value <= 100.0;
}

static bool valid_device_name(const char *name)
{
	const char *cursor;

	if (strncmp(name, "zram", 4))
		return false;
	for (cursor = name + 4; *cursor; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return false;
	}
	return cursor != name + 4;
}

static bool read_psi_some_avg10(const char *path, double *value)
{
	char line[LINE_SIZE];
	FILE *file = fopen(path, "r");
	bool found = false;

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		char *cursor;
		if (strncmp(line, "some ", 5))
			continue;
		cursor = strstr(line, "avg10=");
		if (cursor) {
			char *end;
			cursor += strlen("avg10=");
			errno = 0;
			*value = strtod(cursor, &end);
			found = !errno && end != cursor && isfinite(*value) &&
				*value >= 0.0 && *value <= 100.0 &&
				(*end == ' ' || *end == '\t' || *end == '\n' ||
				 *end == '\0');
		}
		break;
	}
	fclose(file);
	return found;
}

static bool read_zram_mem_used(const char *path,
			       unsigned long long *mem_used)
{
	char line[LINE_SIZE];
	char *cursor;
	char *end;
	unsigned long long values[3];
	FILE *file = fopen(path, "r");
	int i;

	if (!file)
		return false;
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return false;
	}
	fclose(file);
	cursor = line;
	for (i = 0; i < 3; i++) {
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		errno = 0;
		values[i] = strtoull(cursor, &end, 10);
		if (errno || end == cursor)
			return false;
		cursor = end;
	}
	*mem_used = values[2];
	return true;
}

static bool file_has_content(const char *path)
{
	char line[LINE_SIZE];
	FILE *file = fopen(path, "r");
	bool has_content = false;

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		char *content = line;
		if (*content == '#') {
			content = strchr(content, ':');
			if (!content)
				continue;
			content++;
		}
		while (*content == ' ' || *content == '\t')
			content++;
		if (*content && *content != '\n') {
			has_content = true;
			break;
		}
	}
	fclose(file);
	return has_content;
}

static bool write_control(const char *path, const char *value,
			  bool apply, const char *label)
{
	FILE *file;
	if (!apply) {
		printf("DRY-RUN write %s: %s\n", label, value);
		return true;
	}
	file = fopen(path, "w");
	if (!file) {
		fprintf(stderr, "ERROR opening %s: %s\n", path,
			strerror(errno));
		return false;
	}
	if (fprintf(file, "%s\n", value) < 0) {
		int saved_errno = errno;
		fclose(file);
		errno = saved_errno;
		fprintf(stderr, "ERROR writing %s: %s\n", path,
			strerror(errno));
		return false;
	}
	if (fclose(file)) {
		fprintf(stderr, "ERROR writing %s: %s\n", path,
			strerror(errno));
		return false;
	}
	printf("APPLY write %s: %s\n", label, value);
	return true;
}

static void sleep_seconds(unsigned int seconds)
{
	struct timespec delay = {
		.tv_sec = seconds,
		.tv_nsec = 0,
	};
	while (nanosleep(&delay, &delay) && errno == EINTR)
		;
}

static bool sample(const struct options *options, time_t *last_action)
{
	char memory_psi_path[PATH_MAX];
	char cpu_psi_path[PATH_MAX];
	char mm_stat_path[PATH_MAX];
	char recomp_algorithm_path[PATH_MAX];
	char idle_path[PATH_MAX];
	char recompress_path[PATH_MAX];
	char command[128];
	double memory_psi, cpu_psi;
	unsigned long long mem_used;
	time_t now = time(NULL);
	int rc;

	rc = snprintf(memory_psi_path, sizeof(memory_psi_path),
		      "%s/pressure/memory", options->proc_root);
	if (rc < 0 || (size_t)rc >= sizeof(memory_psi_path))
		return false;
	rc = snprintf(cpu_psi_path, sizeof(cpu_psi_path),
		      "%s/pressure/cpu", options->proc_root);
	if (rc < 0 || (size_t)rc >= sizeof(cpu_psi_path))
		return false;
	rc = snprintf(mm_stat_path, sizeof(mm_stat_path),
		      "%s/block/%s/mm_stat", options->sysfs_root,
		      options->device);
	if (rc < 0 || (size_t)rc >= sizeof(mm_stat_path))
		return false;
	rc = snprintf(recomp_algorithm_path, sizeof(recomp_algorithm_path),
		      "%s/block/%s/recomp_algorithm", options->sysfs_root,
		      options->device);
	if (rc < 0 || (size_t)rc >= sizeof(recomp_algorithm_path))
		return false;
	rc = snprintf(idle_path, sizeof(idle_path),
		      "%s/block/%s/idle", options->sysfs_root,
		      options->device);
	if (rc < 0 || (size_t)rc >= sizeof(idle_path))
		return false;
	rc = snprintf(recompress_path, sizeof(recompress_path),
		      "%s/block/%s/recompress", options->sysfs_root,
		      options->device);
	if (rc < 0 || (size_t)rc >= sizeof(recompress_path))
		return false;

	if (!read_psi_some_avg10(memory_psi_path, &memory_psi) ||
	    !read_psi_some_avg10(cpu_psi_path, &cpu_psi) ||
	    !read_zram_mem_used(mm_stat_path, &mem_used)) {
		fprintf(stderr, "ERROR reading PSI or ZRAM stats; no action taken\n");
		return false;
	}

	printf("memory_psi_some_avg10=%.2f cpu_psi_some_avg10=%.2f "
	       "zram_mem_used_total=%llu ", memory_psi, cpu_psi, mem_used);
	if (memory_psi < options->memory_psi_limit) {
		puts("decision=skip reason=memory-pressure-below-threshold");
		return true;
	}
	if (cpu_psi > options->cpu_psi_limit) {
		puts("decision=skip reason=cpu-pressure-above-threshold");
		return true;
	}
	if (mem_used < options->min_used_bytes) {
		puts("decision=skip reason=zram-usage-below-threshold");
		return true;
	}
	if (now - *last_action < (time_t)options->cooldown) {
		puts("decision=skip reason=cooldown");
		return true;
	}
	if (!file_has_content(recomp_algorithm_path)) {
		puts("decision=skip reason=no-secondary-algorithm-configured");
		return true;
	}
	rc = snprintf(command, sizeof(command),
		      "type=idle priority=%u max_pages=%u",
		      options->priority, options->max_pages);
	if (rc < 0 || (size_t)rc >= sizeof(command))
		return false;

	puts("decision=recompress reason=memory-pressure-and-cpu-headroom");
	if (!write_control(idle_path, "all", options->apply, "idle"))
		return false;
	if (!write_control(recompress_path, command, options->apply,
			   "recompress"))
		return false;
	*last_action = now;
	return true;
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "memory-psi", required_argument, NULL, 'm' },
		{ "cpu-psi", required_argument, NULL, 'c' },
		{ "min-used-mib", required_argument, NULL, 'u' },
		{ "max-pages", required_argument, NULL, 'p' },
		{ "priority", required_argument, NULL, 'r' },
		{ "interval", required_argument, NULL, 'i' },
		{ "cooldown", required_argument, NULL, 'w' },
		{ "sysfs-root", required_argument, NULL, 's' },
		{ "proc-root", required_argument, NULL, 'P' },
		{ "apply", no_argument, NULL, 'a' },
		{ "once", no_argument, NULL, 'o' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct options options = {
		.sysfs_root = DEFAULT_SYSFS_ROOT,
		.proc_root = DEFAULT_PROC_ROOT,
		.device = DEFAULT_DEVICE,
		.interval = DEFAULT_INTERVAL,
		.cooldown = DEFAULT_COOLDOWN,
		.max_pages = DEFAULT_MAX_PAGES,
		.priority = DEFAULT_PRIORITY,
		.min_used_bytes = (unsigned long long)DEFAULT_MIN_USED_MIB << 20,
		.memory_psi_limit = DEFAULT_MEMORY_PSI,
		.cpu_psi_limit = DEFAULT_CPU_PSI,
	};
	unsigned int parsed;
	int option;
	time_t last_action = 0;

	while ((option = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
		switch (option) {
		case 'd': options.device = optarg; break;
		case 'm':
			if (!parse_double(optarg, &options.memory_psi_limit))
				goto invalid_option;
			break;
		case 'c':
			if (!parse_double(optarg, &options.cpu_psi_limit))
				goto invalid_option;
			break;
		case 'u':
			if (!parse_uint(optarg, &parsed) || parsed > UINT_MAX / (1U << 20))
				goto invalid_option;
			options.min_used_bytes = (unsigned long long)parsed << 20;
			break;
		case 'p':
			if (!parse_uint(optarg, &options.max_pages) || !options.max_pages)
				goto invalid_option;
			break;
		case 'r':
			if (!parse_uint(optarg, &options.priority) || !options.priority)
				goto invalid_option;
			break;
		case 'i':
			if (!parse_uint(optarg, &options.interval) || !options.interval)
				goto invalid_option;
			break;
		case 'w':
			if (!parse_uint(optarg, &options.cooldown))
				goto invalid_option;
			break;
		case 's': options.sysfs_root = optarg; break;
		case 'P': options.proc_root = optarg; break;
		case 'a': options.apply = true; break;
		case 'o': options.once = true; break;
		case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
		default: goto invalid_option;
		}
	}
	if (optind != argc || !valid_device_name(options.device))
		goto invalid_option;

	do {
		if (!sample(&options, &last_action))
			return EXIT_FAILURE;
		if (!options.once)
			sleep_seconds(options.interval);
	} while (!options.once);
	return EXIT_SUCCESS;

invalid_option:
	fprintf(stderr, "Invalid option or value.\n");
	usage(stderr, argv[0]);
	return EXIT_FAILURE;
}
