#define _POSIX_C_SOURCE 200809L

#include "wfc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef enum {
    ACTION_CHECK,
    ACTION_COMPILE
} action_t;

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s check protocol.wf\n", program);
    fprintf(stderr, "       %s protocol.wf --output path/stem\n", program);
}

static const char *plural(size_t count)
{
    return count == 1 ? "" : "s";
}

static char *append_suffix(const char *path, const char *suffix)
{
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);
    char *result;
    if (path_length > SIZE_MAX - suffix_length - 1)
        return NULL;
    result = malloc(path_length + suffix_length + 1);
    if (result == NULL)
        return NULL;
    memcpy(result, path, path_length);
    memcpy(result + path_length, suffix, suffix_length + 1);
    return result;
}

static int read_source(const char *path, char **text, size_t *length)
{
    FILE *file;
    long size;
    size_t got;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "%s: error: %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: error: %s\n", path, strerror(errno));
        fclose(file);
        return 0;
    }
    *text = malloc((size_t)size + 1);
    if (*text == NULL) {
        fprintf(stderr, "%s: error: out of memory\n", path);
        fclose(file);
        return 0;
    }
    got = fread(*text, 1, (size_t)size, file);
    if (got != (size_t)size || ferror(file)) {
        fprintf(stderr, "%s: error reading file\n", path);
        free(*text);
        *text = NULL;
        fclose(file);
        return 0;
    }
    (*text)[got] = 0;
    *length = got;
    if (fclose(file) != 0) {
        fprintf(stderr, "%s: error: %s\n", path, strerror(errno));
        free(*text);
        *text = NULL;
        return 0;
    }
    return 1;
}

static int make_parent_directories(const char *path)
{
    char *copy = wfc_duplicate(path, strlen(path));
    char *scan;
    if (copy == NULL)
        return 0;
    for (scan = copy + 1; *scan != 0; scan++) {
        if (*scan != '/')
            continue;
        *scan = 0;
        if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
            free(copy);
            return 0;
        }
        *scan = '/';
    }
    free(copy);
    return 1;
}

static int write_new_file(const char *path, const void *data, size_t length)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    int sync_result;
    int close_result;
    const uint8_t *cursor = data;
    if (descriptor < 0)
        return 0;
    while (length != 0) {
        ssize_t written = write(descriptor, cursor, length);
        if (written <= 0) {
            if (written < 0 && errno == EINTR)
                continue;
            if (written == 0)
                errno = EIO;
            close(descriptor);
            return 0;
        }
        cursor += written;
        length -= (size_t)written;
    }
    sync_result = fsync(descriptor);
    close_result = close(descriptor);
    return sync_result == 0 && close_result == 0;
}

static int move_existing(const char *path, const char *backup, int *existed)
{
    struct stat status;
    *existed = 0;
    if (lstat(path, &status) != 0) {
        if (errno == ENOENT)
            return 1;
        return 0;
    }
    if (!S_ISREG(status.st_mode)) {
        errno = EINVAL;
        return 0;
    }
    if (rename(path, backup) != 0)
        return 0;
    *existed = 1;
    return 1;
}

static void restore(const char *path, const char *backup, int existed)
{
    unlink(path);
    if (existed)
        rename(backup, path);
}

static int write_outputs(const char *stem, const wfc_generated_t *generated)
{
    char nonce[64];
    char old_nonce[72];
    char *header = append_suffix(stem, ".h");
    char *binary = append_suffix(stem, ".wi");
    char *header_temp = NULL;
    char *binary_temp = NULL;
    char *header_backup = NULL;
    char *binary_backup = NULL;
    int had_header = 0;
    int had_binary = 0;
    int success = 0;

    snprintf(nonce, sizeof(nonce), ".wfc-%ld", (long)getpid());
    snprintf(old_nonce, sizeof(old_nonce), "%s.old", nonce);
    if (header == NULL || binary == NULL ||
        (header_temp = append_suffix(header, nonce)) == NULL ||
        (binary_temp = append_suffix(binary, nonce)) == NULL ||
        (header_backup = append_suffix(header, old_nonce)) == NULL ||
        (binary_backup = append_suffix(binary, old_nonce)) == NULL)
        goto done;
    if (!make_parent_directories(header))
        goto done;
    unlink(header_temp);
    unlink(binary_temp);
    unlink(header_backup);
    unlink(binary_backup);
    if (!write_new_file(header_temp, generated->header, strlen(generated->header)) ||
        !write_new_file(binary_temp, generated->binary, generated->binary_size))
        goto done;
    if (!move_existing(header, header_backup, &had_header))
        goto done;
    if (!move_existing(binary, binary_backup, &had_binary)) {
        restore(header, header_backup, had_header);
        goto done;
    }
    if (rename(header_temp, header) != 0) {
        restore(header, header_backup, had_header);
        restore(binary, binary_backup, had_binary);
        goto done;
    }
    if (rename(binary_temp, binary) != 0) {
        restore(header, header_backup, had_header);
        restore(binary, binary_backup, had_binary);
        goto done;
    }
    unlink(header_backup);
    unlink(binary_backup);
    success = 1;

done:
    if (!success)
        fprintf(stderr, "%s: error writing generated files: %s\n",
                stem, errno == 0 ? "out of memory" : strerror(errno));
    if (header_temp != NULL) unlink(header_temp);
    if (binary_temp != NULL) unlink(binary_temp);
    free(header);
    free(binary);
    free(header_temp);
    free(binary_temp);
    free(header_backup);
    free(binary_backup);
    return success;
}

int main(int argc, char **argv)
{
    action_t action;
    const char *input;
    const char *output = NULL;
    char *text = NULL;
    size_t length = 0;
    wfc_protocol_t protocol;
    wfc_summary_t summary;
    wfc_generated_t generated;
    wfc_error_t error;
    int index;
    int result = EXIT_FAILURE;

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc == 2 && (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0)) {
        puts("wfc 0.1.0");
        return EXIT_SUCCESS;
    }
    if (argc < 2) {
        fprintf(stderr, "error: expected a .wf file or the `check` command\n");
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "check") == 0) {
        if (argc != 3) {
            fprintf(stderr, "error: `check` requires exactly one .wf file\n");
            usage(argv[0]);
            return 2;
        }
        action = ACTION_CHECK;
        input = argv[2];
    } else {
        action = ACTION_COMPILE;
        input = argv[1];
        for (index = 2; index < argc; index++) {
            if (strcmp(argv[index], "-o") == 0 || strcmp(argv[index], "--output") == 0) {
                if (++index == argc) {
                    fprintf(stderr, "error: `--output` requires a path stem\n");
                    usage(argv[0]);
                    return 2;
                }
                if (output != NULL) {
                    fprintf(stderr, "error: `--output` may be specified only once\n");
                    usage(argv[0]);
                    return 2;
                }
                output = argv[index];
            } else {
                fprintf(stderr, "error: unknown option `%s`\n", argv[index]);
                usage(argv[0]);
                return 2;
            }
        }
        if (output == NULL) {
            fprintf(stderr, "error: compilation requires `--output path-stem`\n");
            usage(argv[0]);
            return 2;
        }
    }

    memset(&protocol, 0, sizeof(protocol));
    memset(&generated, 0, sizeof(generated));
    if (!read_source(input, &text, &length))
        goto done;
    if (!wfc_source_parse(text, length, &protocol, &error) ||
        !wfc_check(&protocol, &summary, &error)) {
        fprintf(stderr, "%s:%zu:%zu: error: %s\n", input,
                error.location.line, error.location.column, error.message);
        goto done;
    }
    if (action == ACTION_CHECK) {
        printf("%s: OK: protocol `%s`, %zu message%s, %zu vector%s, %zu layout octet%s\n",
               input, protocol.name, summary.messages, plural(summary.messages),
               summary.vectors, plural(summary.vectors), summary.layout_octets,
               plural(summary.layout_octets));
        result = EXIT_SUCCESS;
        goto done;
    }
    if (!wfc_generate(&protocol, &generated, &error)) {
        fprintf(stderr, "%s:%zu:%zu: error: %s\n", input,
                error.location.line, error.location.column, error.message);
        goto done;
    }
    if (!write_outputs(output, &generated))
        goto done;
    printf("%s: compiled protocol `%s` (%zu message%s, %zu vector%s) -> %s.h, %s.wi\n",
           input, protocol.name, summary.messages, plural(summary.messages),
           summary.vectors, plural(summary.vectors), output, output);
    result = EXIT_SUCCESS;

done:
    free(text);
    wfc_generated_free(&generated);
    wfc_protocol_free(&protocol);
    return result;
}
