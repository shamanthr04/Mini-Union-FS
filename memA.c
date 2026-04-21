/*
 * Mini-UnionFS — Member A Implementation
 * Functions: resolve_path, copy_up, getattr, readdir
 * Core responsibility: Path resolution algorithm + Copy-on-Write core
 */

#define FUSE_USE_VERSION 31
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

/* =========================================
   PATH HELPER FUNCTIONS
   ========================================= */

/* Join base dir + FUSE path → real filesystem path */
static void make_path(char *out, const char *base, const char *fuse_path) {
    snprintf(out, PATH_MAX, "%s%s", base, fuse_path);
}

/* Build the whiteout path for a FUSE path */
static void make_whiteout_path(char *out, const char *fuse_path) {
    char tmp[PATH_MAX];
    strncpy(tmp, fuse_path, PATH_MAX - 1);
    char *slash = strrchr(tmp, '/');
    char *filename = slash + 1;
    if (slash == tmp) {
        snprintf(out, PATH_MAX, "%s/.wh.%s", STATE->upper_dir, filename);
    } else {
        *slash = '\0';
        snprintf(out, PATH_MAX, "%s%s/.wh.%s", STATE->upper_dir, tmp, filename);
    }
}

/* resolve_path — the heart of the file system. */
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

/* =========================================
   COPY-ON-WRITE (copy_up)
   ========================================= */

/* Called automatically before any write to a lower-layer file. */
static int copy_up(const char *fuse_path) {
    char lower[PATH_MAX], upper[PATH_MAX];
    make_path(lower, STATE->lower_dir, fuse_path);
    make_path(upper, STATE->upper_dir, fuse_path);

    if (access(upper, F_OK) == 0) return 0;  /* already in upper, nothing to do */

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

/* =========================================
   FUSE CALLBACKS
   ========================================= */

/* getattr — called for every ls and stat */
static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi) {
    (void) fi;
    char real[PATH_MAX];
    int ret = resolve_path(path, real);
    if (ret < 0) return ret;
    if (lstat(real, stbuf) < 0) return -errno;
    return 0;
}

/* readdir — merges upper and lower listings, hides whiteouts */
static int unionfs_readdir(const char *path, void *buf,
                           fuse_fill_dir_t filler, off_t offset,
                           struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void) offset; (void) fi; (void) flags;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    char seen[256][NAME_MAX + 1];
    int seen_count = 0;

    /* Read upper first — takes precedence */
    char upper[PATH_MAX];
    make_path(upper, STATE->upper_dir, path);
    DIR *du = opendir(upper);
    if (du) {
        struct dirent *de;
        while ((de = readdir(du)) != NULL) {
            if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
            if (strncmp(de->d_name, ".wh.", 4) == 0) {
                /* Record the name being hidden, skip the .wh. file itself */
                strncpy(seen[seen_count++], de->d_name + 4, NAME_MAX);
                continue;
            }
            filler(buf, de->d_name, NULL, 0, 0);
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
            if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
            int found = 0;
            for (int i = 0; i < seen_count; i++)
                if (!strcmp(seen[i], de->d_name)) { found = 1; break; }
            if (!found) filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dl);
    }
    return 0;
}