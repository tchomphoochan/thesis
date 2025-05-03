#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmlog.h"

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <-b|-t> <log>\n", argv[0]);
    fprintf(stderr, "  -b indicates binary input, text output\n");
    fprintf(stderr, "  -t indicates text input, binary output\n");
    exit(1);
  }

  const char *flag = argv[1];
  const char *filename = argv[2];

  if (strcmp(flag, "-b") == 0) {
    FILE *file = fopen(filename, "rb");
    pmlog_read(file, NULL, NULL);
    pmlog_dump_text(stdout);
  } else if (strcmp(flag, "-t") == 0) {
    fprintf(stderr, "Not implemented\n");
  } else {
    fprintf(stderr, "Invalid flag\n");
  }
}
