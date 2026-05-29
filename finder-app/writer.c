#include <stdio.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[]) {
    openlog("writer", LOG_PID, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Usage: %s <string> <file>\n", argv[0]);
        return 1;
    }

    const char *file_path = argv[1];
    const char *string_to_write = argv[2];

    FILE *file = fopen(file_path, "w");
    if (file == NULL) {
        syslog(LOG_ERR, "Error opening file: %s\n", strerror(errno));
        return 1;
    }

    fprintf(file, "%s\n", string_to_write);
    fclose(file);

    syslog(LOG_DEBUG, "Writing '%s' to '%s'\n", string_to_write, file_path);

    closelog();
    return 0;
}