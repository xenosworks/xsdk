// The PC harness's command line over the exFAT core (docs/scope/77-exfat.md, step 2): one command per run on a disk
// image or block device, reached through pread and pwrite, mounted and synced around it. Names on the command line
// are Latin-1 bytes, as the console's are. EXFAT_HOST_FAIL_AFTER=<n> makes the device fail its n-th write and every
// one after, as a power cut would; EXFAT_HOST_SECTOR=<bytes> sets the sector size, 512 by default.
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xsdk/exfat.h"

enum { CHUNK_DEFAULT = 1 << 20 };
// 1970-01-01 as a FILETIME.
static const uint64_t FILETIME_1970 = 116444736000000000ULL;

typedef struct {
	int fd;
	uint32_t sector_bytes;
	uint64_t writes;
	uint64_t fail_after;
} Image;

static ExfatVolume volume;

static int image_read(void *context, uint64_t sector, uint32_t count, void *buffer)
{
	const Image *const image = context;
	size_t left = (size_t)count * image->sector_bytes;
	off_t at = (off_t)(sector * image->sector_bytes);
	uint8_t *out = buffer;
	while (left) {
		const ssize_t got = pread(image->fd, out, left, at);
		if (got <= 0)
			return -1;
		out += got;
		at += got;
		left -= (size_t)got;
	}
	return 0;
}

static int image_write(void *context, uint64_t sector, uint32_t count, const void *buffer)
{
	Image *const image = context;
	if (image->fail_after && image->writes >= image->fail_after)
		return -1;
	image->writes++;
	size_t left = (size_t)count * image->sector_bytes;
	off_t at = (off_t)(sector * image->sector_bytes);
	const uint8_t *in = buffer;
	while (left) {
		const ssize_t put = pwrite(image->fd, in, left, at);
		if (put <= 0)
			return -1;
		in += put;
		at += put;
		left -= (size_t)put;
	}
	return 0;
}

static uint64_t image_now(void *context)
{
	(void)context;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return FILETIME_1970 + (uint64_t)now.tv_sec * 10000000 + (uint64_t)now.tv_nsec / 100;
}

static void fail(ExfatStatus status, const char *what)
{
	fprintf(stderr, "error: %s: %s\n", what, xsdk_exfat_status_name(status));
	exit(1);
}

static void fail_errno(const char *what)
{
	fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void need(int argc, int wanted, const char *usage)
{
	if (argc < wanted) {
		fprintf(stderr, "usage: exfat_host <image> %s\n", usage);
		exit(2);
	}
}

static ExfatNode lookup(const char *path)
{
	ExfatNode node;
	const ExfatStatus status = xsdk_exfat_lookup(&volume, path, &node);
	if (status != EXFAT_OK)
		fail(status, path);
	return node;
}

static void command_info(void)
{
	printf("label=%s serial=%08" PRIx32 " sector=%" PRIu32 " cluster=%" PRIu64 " clusters=%" PRIu32 " free=%" PRIu32
		   " root=%" PRIu32 " dirty=%d\n",
		volume.label, volume.serial, 1u << volume.sector_shift, (uint64_t)1 << volume.cluster_shift,
		volume.cluster_count, volume.free_clusters, volume.root_cluster, volume.dirty);
}

static void command_ls(const char *path)
{
	const ExfatNode dir = lookup(path);
	ExfatDirectory listing;
	ExfatStatus status = xsdk_exfat_open_directory(&volume, &dir, &listing);
	if (status != EXFAT_OK)
		fail(status, path);
	ExfatNode entry;
	while ((status = xsdk_exfat_read_directory(&volume, &listing, &entry)) == EXFAT_OK)
		printf("%c %" PRIu64 " %s\n", entry.attributes & 0x10 ? 'D' : 'F', entry.size_bytes, entry.name);
	if (status != EXFAT_NOT_FOUND)
		fail(status, path);
}

static void command_stat(const char *path)
{
	const ExfatNode node = lookup(path);
	printf("attributes=%" PRIx16 " size=%" PRIu64 " valid=%" PRIu64 " created=%" PRIu64 " modified=%" PRIu64
		   " accessed=%" PRIu64 " first=%" PRIu32 " contiguous=%d\n",
		node.attributes, node.size_bytes, node.valid_bytes, node.created, node.modified, node.accessed,
		node.first_cluster, node.contiguous);
}

static void command_get(const char *path, const char *out_path)
{
	ExfatNode node = lookup(path);
	FILE *const out = fopen(out_path, "wb");
	if (!out)
		fail_errno(out_path);
	uint8_t *const chunk = malloc(CHUNK_DEFAULT);
	for (uint64_t offset = 0;;) {
		uint32_t got;
		const ExfatStatus status = xsdk_exfat_read(&volume, &node, offset, chunk, CHUNK_DEFAULT, &got);
		if (status != EXFAT_OK)
			fail(status, path);
		if (!got)
			break;
		if (fwrite(chunk, 1, got, out) != got)
			fail_errno(out_path);
		offset += got;
	}
	if (fclose(out))
		fail_errno(out_path);
	free(chunk);
}

// The file at `path`, created when missing.
static ExfatNode open_or_create(const char *path)
{
	ExfatNode node;
	ExfatStatus status = xsdk_exfat_lookup(&volume, path, &node);
	if (status == EXFAT_NOT_FOUND)
		status = xsdk_exfat_create(&volume, path, false, &node);
	if (status != EXFAT_OK)
		fail(status, path);
	return node;
}

// Copies `bytes` of the input from `offset` to the same offset in the file, in chunks.
static void write_range(ExfatNode *node, const char *path, FILE *in, uint64_t offset, uint64_t bytes, uint32_t chunk)
{
	uint8_t *const buffer = malloc(chunk);
	if (fseeko(in, (off_t)offset, SEEK_SET))
		fail_errno("input");
	while (bytes) {
		const size_t want = bytes < chunk ? (size_t)bytes : chunk;
		if (fread(buffer, 1, want, in) != want)
			fail_errno("input");
		const ExfatStatus status = xsdk_exfat_write(&volume, node, offset, buffer, (uint32_t)want);
		if (status != EXFAT_OK)
			fail(status, path);
		offset += want;
		bytes -= want;
	}
	free(buffer);
}

static FILE *open_input(const char *in_path, uint64_t *size)
{
	FILE *const in = fopen(in_path, "rb");
	if (!in || fseeko(in, 0, SEEK_END))
		fail_errno(in_path);
	*size = (uint64_t)ftello(in);
	return in;
}

static void command_put(const char *path, const char *in_path, uint32_t chunk)
{
	uint64_t size;
	FILE *const in = open_input(in_path, &size);
	ExfatNode node = open_or_create(path);
	write_range(&node, path, in, 0, size, chunk);
	const ExfatStatus status = xsdk_exfat_resize(&volume, &node, size);
	if (status != EXFAT_OK)
		fail(status, path);
	fclose(in);
}

static void command_write(const char *path, const char *in_path, uint64_t offset, uint64_t bytes)
{
	uint64_t size;
	FILE *const in = open_input(in_path, &size);
	ExfatNode node = open_or_create(path);
	write_range(&node, path, in, offset, bytes, CHUNK_DEFAULT);
	fclose(in);
}

static void command_format(Image *image, uint64_t sectors, const char *label, uint32_t serial)
{
	const ExfatDevice device = { image_read, image_write, image_now, image, image->sector_bytes, 0, sectors };
	const ExfatStatus status = xsdk_exfat_format(&device, label, serial);
	if (status != EXFAT_OK)
		fail(status, "format");
}

int main(int argc, char **argv)
{
	need(argc, 3, "info|ls|stat|get|put|write|mkdir|rm|mv|truncate|attrib|format ...");
	const char *const image_path = argv[1];
	const char *const command = argv[2];
	Image image = { -1, 512, 0, 0 };
	if (getenv("EXFAT_HOST_SECTOR"))
		image.sector_bytes = (uint32_t)strtoul(getenv("EXFAT_HOST_SECTOR"), NULL, 0);
	if (getenv("EXFAT_HOST_FAIL_AFTER"))
		image.fail_after = strtoull(getenv("EXFAT_HOST_FAIL_AFTER"), NULL, 0);
	image.fd = open(image_path, O_RDWR);
	if (image.fd < 0)
		fail_errno(image_path);
	const off_t end = lseek(image.fd, 0, SEEK_END);
	if (end < 0)
		fail_errno(image_path);
	const uint64_t sectors = (uint64_t)end / image.sector_bytes;
	if (strcmp(command, "format") == 0) {
		need(argc, 5, "format <label> <serial-hex>");
		command_format(&image, sectors, argv[3], (uint32_t)strtoul(argv[4], NULL, 16));
		return 0;
	}
	ExfatDevice device = { image_read, image_write, image_now, &image, image.sector_bytes, 0, sectors };
	ExfatStatus status = xsdk_exfat_locate(&device);
	if (status == EXFAT_OK)
		status = xsdk_exfat_mount(&volume, &device);
	if (status != EXFAT_OK)
		fail(status, "mount");
	if (strcmp(command, "info") == 0) {
		command_info();
	} else if (strcmp(command, "ls") == 0) {
		need(argc, 4, "ls <path>");
		command_ls(argv[3]);
	} else if (strcmp(command, "stat") == 0) {
		need(argc, 4, "stat <path>");
		command_stat(argv[3]);
	} else if (strcmp(command, "get") == 0) {
		need(argc, 5, "get <path> <out>");
		command_get(argv[3], argv[4]);
	} else if (strcmp(command, "put") == 0) {
		need(argc, 5, "put <path> <in> [chunk]");
		command_put(argv[3], argv[4], argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : CHUNK_DEFAULT);
	} else if (strcmp(command, "write") == 0) {
		need(argc, 7, "write <path> <in> <offset> <bytes>");
		command_write(argv[3], argv[4], strtoull(argv[5], NULL, 0), strtoull(argv[6], NULL, 0));
	} else if (strcmp(command, "mkdir") == 0) {
		need(argc, 4, "mkdir <path>");
		ExfatNode node;
		status = xsdk_exfat_create(&volume, argv[3], true, &node);
	} else if (strcmp(command, "rm") == 0) {
		need(argc, 4, "rm <path>");
		const ExfatNode node = lookup(argv[3]);
		status = xsdk_exfat_remove(&volume, &node);
	} else if (strcmp(command, "mv") == 0) {
		need(argc, 5, "mv <path> <new-path>");
		ExfatNode node = lookup(argv[3]);
		status = xsdk_exfat_rename(&volume, &node, argv[4]);
	} else if (strcmp(command, "truncate") == 0) {
		need(argc, 5, "truncate <path> <size>");
		ExfatNode node = lookup(argv[3]);
		status = xsdk_exfat_resize(&volume, &node, strtoull(argv[4], NULL, 0));
	} else if (strcmp(command, "attrib") == 0) {
		need(argc, 5, "attrib <path> <hex>");
		ExfatNode node = lookup(argv[3]);
		node.attributes = (uint16_t)strtoul(argv[4], NULL, 16);
		status = xsdk_exfat_update(&volume, &node);
	} else {
		need(0, 1, "info|ls|stat|get|put|write|mkdir|rm|mv|truncate|attrib|format ...");
	}
	if (status != EXFAT_OK)
		fail(status, argv[3]);
	status = xsdk_exfat_unmount(&volume);
	if (status != EXFAT_OK)
		fail(status, "unmount");
	return 0;
}
