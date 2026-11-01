// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>

static int checked(int status, struct archive* archive, const char* operation) {
  if (status == ARCHIVE_OK) return 1;
  fprintf(stderr, "%s: %s\n", operation, archive_error_string(archive));
  return 0;
}

int main(void) {
#ifdef _WIN32
  if (!setlocale(LC_CTYPE, ".UTF8")) { fputs("UTF-8 locale unavailable\n", stderr); return 1; }
#else
  if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8")) { fputs("UTF-8 locale unavailable\n", stderr); return 1; }
#endif
  char buffer[16384], content[32];
  size_t used = 0;
  const char* name = "folder/Gr\xc3\xbc\xc3\x9f" "e.txt";
#ifdef __APPLE__
  // libarchive's charset conversion deliberately returns NFD on macOS.
  // Keep an exact UTF-8 check, including the combining diaeresis.
  const char* expected_name = "folder/Gru\xcc\x88\xc3\x9f" "e.txt";
#else
  const char* expected_name = name;
#endif
  const char* payload = "archive roundtrip\n";
  struct archive* writer = archive_write_new();
  struct archive_entry* entry = archive_entry_new();
  if (!writer || !entry || archive_version_number() != 3008010) {
    fprintf(stderr, "allocation/version check failed: %d\n", archive_version_number()); return 1;
  }
  if (!checked(archive_write_set_format_pax_restricted(writer), writer, "TAR format") ||
      !checked(archive_write_add_filter_none(writer), writer, "uncompressed filter") ||
      !checked(archive_write_set_bytes_per_block(writer, 512), writer, "block size") ||
      !checked(archive_write_open_memory(writer, buffer, sizeof(buffer), &used), writer, "memory writer")) return 1;
  archive_entry_set_pathname_utf8(entry, name);
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  archive_entry_set_size(entry, (la_int64_t)strlen(payload));
  if (!checked(archive_write_header(writer, entry), writer, "write header") ||
      archive_write_data(writer, payload, strlen(payload)) != (la_ssize_t)strlen(payload) ||
      !checked(archive_write_close(writer), writer, "close writer")) return 1;
  archive_entry_free(entry);
  archive_write_free(writer);

  struct archive* reader = archive_read_new();
  if (!reader || !checked(archive_read_support_filter_none(reader), reader, "read uncompressed") ||
      !checked(archive_read_support_format_tar(reader), reader, "read TAR") ||
      !checked(archive_read_open_memory(reader, buffer, used), reader, "memory reader") ||
      !checked(archive_read_next_header(reader, &entry), reader, "read header")) return 1;
  const char* received_name = archive_entry_pathname_utf8(entry);
  if (!received_name || strcmp(received_name, expected_name)) {
    fprintf(stderr, "pathname mismatch: expected '%s', received '%s'\n", expected_name, received_name ? received_name : "<null>"); return 1;
  }
  if (archive_entry_size(entry) != (la_int64_t)strlen(payload)) {
    fputs("entry size mismatch\n", stderr); return 1;
  }
  la_ssize_t read_size = archive_read_data(reader, content, sizeof(content));
  if (read_size != (la_ssize_t)strlen(payload)) {
    fprintf(stderr, "payload read size mismatch: %lld\n", (long long)read_size); return 1;
  }
  if (memcmp(content, payload, strlen(payload))) { fputs("payload mismatch\n", stderr); return 1; }
  if (archive_read_next_header(reader, &entry) != ARCHIVE_EOF) { fputs("expected end of archive\n", stderr); return 1; }
  archive_read_free(reader);
  puts("libarchive 3.8.10 static UTF-8 TAR roundtrip passed");
  return 0;
}
