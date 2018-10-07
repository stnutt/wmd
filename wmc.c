#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/un.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>

void die(const char *error)
{
    fprintf(stderr, "%s\n", error);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    char* dpy;
    char *sock_dir;
    int sock_fd;
    struct sockaddr_un sock_addr;

    if (argc == 1)
    {
        die("");
    }

    if (!(dpy = getenv("DISPLAY")))
        die("");

    if (!(sock_dir = getenv("XDG_RUNTIME_DIR")))
        sock_dir = "/tmp";

    sock_addr.sun_family = AF_UNIX;
    snprintf(sock_addr.sun_path, sizeof(sock_addr.sun_path), "%s/wmd%s", sock_dir, dpy);
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(sock_fd, (struct sockaddr *) &sock_addr, sizeof(sock_addr)) == 1)
    {
        die("");
    }

    int size = 1024;
    char *buf;
    int len = 0;

    buf =  malloc(size);

    for (int i = 1; i < argc; i++)
    {
        int j = 0;
        do {
            if (len == size)
            {
                size *= 2;
                buf = realloc(buf, size);
            }
            buf[len] = argv[i][j++];
        } while (buf[len++] != '\0');
    }

    if (send(sock_fd, buf, len, 0) == -1)
    {
        /* die("") */
    }

    int ret = -1;

    while ((len = recv(sock_fd, buf, size, 0)) > 0)
    {
        if (ret == -1 && len >= 1)
        {
            ret = buf[0] - 48;
            fwrite(buf + 1, 1, len - 1, stdout);
        }
        else
        {
            fwrite(buf, 1, len, stdout);
        }
    }

    free(buf);
    close(sock_fd);

    if (ret == -1)
    {
        ret = 1;
    }

    return ret;
}
