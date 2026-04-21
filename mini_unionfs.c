#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

/* Global state: our two layer directories */
struct unionfs_state {
    char *lower_dir;   /* read-only base layer */
    char *upper_dir;   /* read-write container layer */
};

/* Access state from any FUSE callback */
#define STATE ((struct unionfs_state *) fuse_get_context()->private_data)

/* Join base dir + FUSE path → real filesystem path */
static void make_path(char *out, const char *base, const char *fuse_path) {
    snprintf(out, PATH_MAX, "%s%s", base, fuse_path);
}

/* Build the whiteout path for a FUSE path.
 * /config.txt  →  upper_dir/.wh.config.txt
 * /sub/a.txt   →  upper_dir/sub/.wh.a.txt  */
static void make_whiteout_path(char *out, const char *fuse_path) {
    char tmp[PATH_MAX];
    strncpy(tmp, fuse_path, PATH_MAX - 1);
    tmp[PATH_MAX - 1] = '\0';
    char *slash = strrchr(tmp, '/');
    char *filename = slash + 1;
    if (slash == tmp) {
        snprintf(out, PATH_MAX, "%s/.wh.%s", STATE->upper_dir, filename);
    } else {
        *slash = '\0';
        snprintf(out, PATH_MAX, "%s%s/.wh.%s", STATE->upper_dir, tmp, filename);
    }
}

/* resolve_path — the heart of the file system.
 * 1. Whiteout exists in upper → return -ENOENT (file was deleted)
 * 2. File exists in upper     → use upper copy
 * 3. File exists in lower     → use lower copy
 * 4. Not found anywhere       → return -ENOENT               */
static int resolve_path(const char *fuse_path, char *out) {
    char upper[PATH_MAX], lower[PATH_MAX], wh[PATH_MAX];
    make_path(upper, STATE->upper_dir, fuse_path);
    make_path(lower, STATE->lower_dir, fuse_path);
    make_whiteout_path(wh, fuse_path);

    if (access(wh, F_OK) == 0)    return -ENOENT;  /* whited out */
    if (access(upper, F_OK) == 0) { strncpy(out, upper, PATH_MAX-1); return 0; }
    if (access(lower, F_OK) == 0) { strncpy(out, lower, PATH_MAX-1); return 0; }
    return -ENOENT;
}

static int copy_up(const char *fuse_path) {
    char lower[PATH_MAX], upper[PATH_MAX];
    make_path(lower, STATE->lower_dir, fuse_path);
    make_path(upper, STATE->upper_dir, fuse_path);

    if (access(upper, F_OK) == 0) return 0;  /* already in upper */

    int src = open(lower, O_RDONLY);
    if (src < 0) return -errno;

    struct stat st;
    fstat(src, &st);

    int dst = open(upper, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst < 0) { close(src); return -errno; }

    char buf[4096];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        if (write(dst, buf, n) != n) {
            close(src); close(dst); return -EIO;
        }
    }
    close(src);
    close(dst);
    return 0;
}

/* getattr — FUSE 2.x signature: no fuse_file_info* parameter */
static int unionfs_getattr(const char *path, struct stat *stbuf) {
    char real[PATH_MAX];
    int ret = resolve_path(path, real);
    if (ret < 0) return ret;
    if (lstat(real, stbuf) < 0) return -errno;
    return 0;
}

/* readdir — FUSE 2.x signature: no fuse_readdir_flags parameter,
 * filler takes 4 args not 5 */
static int unionfs_readdir(const char *path, void *buf,
        fuse_fill_dir_t filler, off_t offset,
        struct fuse_file_info *fi) {
    (void) offset; (void) fi;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    char seen[256][NAME_MAX + 1];
    int seen_count = 0;

    /* Read upper first — takes precedence */
    char upper[PATH_MAX];
    make_path(upper, STATE->upper_dir, path);
    DIR *du = opendir(upper);
    if (du) {
        struct dirent *de;
        while ((de = readdir(du)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (strncmp(de->d_name, ".wh.", 4) == 0) {
                /* Record the name being hidden, skip the .wh. file itself */
                strncpy(seen[seen_count++], de->d_name + 4, NAME_MAX);
                continue;
            }
            filler(buf, de->d_name, NULL, 0);
            strncpy(seen[seen_count++], de->d_name, NAME_MAX);
        }
        closedir(du);
    }

    /* Read lower — skip anything already seen */
    char lower[PATH_MAX];
    make_path(lower, STATE->lower_dir, path);
    DIR *dl = opendir(lower);
    if (dl) {
        struct dirent *de;
        while ((de = readdir(dl)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            int found = 0;
            for (int i = 0; i < seen_count; i++)
                if (!strcmp(seen[i], de->d_name)) { found = 1; break; }
            if (!found) filler(buf, de->d_name, NULL, 0);
        }
        closedir(dl);
    }
    return 0;
}

/* open — triggers CoW if file is opened for writing */
static int unionfs_open(const char *path, struct fuse_file_info *fi) {
    char real[PATH_MAX];
    int ret = resolve_path(path, real);
    if (ret < 0) return ret;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        char upper[PATH_MAX];
        make_path(upper, STATE->upper_dir, path);
        if (access(upper, F_OK) != 0) {
            ret = copy_up(path);
            if (ret < 0) return ret;
        }
    }
    return 0;
}

/* read — read bytes from whichever layer has the file */
static int unionfs_read(const char *path, char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    char real[PATH_MAX];
    int ret = resolve_path(path, real);
    if (ret < 0) return ret;
    int fd = open(real, O_RDONLY);
    if (fd < 0) return -errno;
    ret = pread(fd, buf, size, offset);
    if (ret < 0) ret = -errno;
    close(fd);
    return ret;
}

/* write — always writes to upper_dir */
static int unionfs_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    char upper[PATH_MAX];
    make_path(upper, STATE->upper_dir, path);
    int fd = open(upper, O_WRONLY);
    if (fd < 0) return -errno;
    int ret = pwrite(fd, buf, size, offset);
    if (ret < 0) ret = -errno;
    close(fd);
    return ret;
}

/* create — new files always land in upper_dir */
static int unionfs_create(const char *path, mode_t mode,
                           struct fuse_file_info *fi) {
    (void) fi;
    char upper[PATH_MAX];
    make_path(upper, STATE->upper_dir, path);
    int fd = open(upper, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

/* unlink — delete from upper if present; create whiteout if in lower */
static int unionfs_unlink(const char *path) {
    char upper[PATH_MAX], lower[PATH_MAX], wh[PATH_MAX];
    make_path(upper, STATE->upper_dir, path);
    make_path(lower, STATE->lower_dir, path);
    make_whiteout_path(wh, path);

    int in_upper = (access(upper, F_OK) == 0);
    int in_lower = (access(lower, F_OK) == 0);

    if (!in_upper && !in_lower) return -ENOENT;
    if (in_upper) unlink(upper);
    if (in_lower) {
        int fd = open(wh, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) return -errno;
        close(fd);
    }
    return 0;
}

/* mkdir / rmdir — always operate on upper_dir */
static int unionfs_mkdir(const char *path, mode_t mode) {
    char upper[PATH_MAX];
    make_path(upper, STATE->upper_dir, path);
    if (mkdir(upper, mode) < 0) return -errno;
    return 0;
}

static int unionfs_rmdir(const char *path) {
    char upper[PATH_MAX];
    make_path(upper, STATE->upper_dir, path);
    if (rmdir(upper) < 0) return -errno;
    return 0;
}

/* truncate — FUSE 2.x signature: no fuse_file_info* parameter */
static int unionfs_truncate(const char *path, off_t size) {
    char upper[PATH_MAX];
    make_path(upper, STATE->upper_dir, path);
    if (access(upper, F_OK) != 0) {
        int ret = copy_up(path);
        if (ret < 0) return ret;
    }
    if (truncate(upper, size) < 0) return -errno;
    return 0;
}

static struct fuse_operations unionfs_oper = {
    .getattr  = unionfs_getattr,
    .readdir  = unionfs_readdir,
    .open     = unionfs_open,
    .read     = unionfs_read,
    .write    = unionfs_write,
    .create   = unionfs_create,
    .unlink   = unionfs_unlink,
    .mkdir    = unionfs_mkdir,
    .rmdir    = unionfs_rmdir,
    .truncate = unionfs_truncate,
};

int main(int argc, char *argv[]) {
    /* Usage: ./mini_unionfs <lower_dir> <upper_dir> <mount_dir> */
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <lower_dir> <upper_dir> <mount_dir>\n", argv[0]);
        return 1;
    }

    struct unionfs_state *state = calloc(1, sizeof(*state));
    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        fprintf(stderr, "Error resolving layer directories\n");
        return 1;
    }

    /* Shift args: remove lower/upper, pass mount_dir to FUSE */
    argv[1] = argv[3];
    argc -= 2;

    return fuse_main(argc, argv, &unionfs_oper, state);
}
