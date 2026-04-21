/*
 * Mini-UnionFS — Member B Implementation
 * Functions: open, read, write, create, truncate
 * Core responsibility: Copy-on-Write (CoW) logic
 *
 * DEPENDS ON: resolve_path() from Member A's code
 * This file is meant to be merged into one main.c with Member A and C's code.
 *
 * How to use:
 *   - Copy these functions into the shared main.c
 *   - Make sure the struct and #defines from Member A are present
 */

/* -----------------------------------------------------------------------
 * CONTEXT: These are the globals/macros Member A defines.
 * DO NOT redefine these in main.c — they already exist from Member A.
 * Shown here for reference only.
 * -----------------------------------------------------------------------
 *
 * struct mini_unionfs_state {
 *     char *lower_dir;
 *     char *upper_dir;
 * };
 * #define STATE ((struct mini_unionfs_state *) fuse_get_context()->private_data)
 *
 * int resolve_path(const char *path, char *resolved_path);
 *   Returns 0 on success (resolved_path filled), -ENOENT if file not found / whited out.
 */

/* -----------------------------------------------------------------------
 * HELPER: copy_up
 * Copies a file from lower_dir to upper_dir (the CoW operation).
 * Called from unionfs_open() when a write is requested on a lower-only file.
 * Member A may also expose this as their copy_up() — if so, use theirs instead.
 * -----------------------------------------------------------------------*/
static int _do_copy_up(const char *path)
{
    struct mini_unionfs_state *state = STATE;

    /* Build source (lower) and destination (upper) full paths */
    char lower_path[PATH_MAX];
    char upper_path[PATH_MAX];
    snprintf(lower_path, PATH_MAX, "%s%s", state->lower_dir, path);
    snprintf(upper_path, PATH_MAX, "%s%s", state->upper_dir, path);

    /* Open source for reading */
    int src_fd = open(lower_path, O_RDONLY);
    if (src_fd < 0)
        return -errno;

    /* Get source metadata so we can preserve permissions */
    struct stat src_stat;
    if (fstat(src_fd, &src_stat) < 0) {
        close(src_fd);
        return -errno;
    }

    /*
     * Ensure parent directories exist in upper_dir.
     * e.g. if path is /subdir/file.txt, we need upper_dir/subdir/ to exist.
     */
    char upper_dir_path[PATH_MAX];
    snprintf(upper_dir_path, PATH_MAX, "%s%s", state->upper_dir, path);
    /* Strip filename to get parent dir */
    char *last_slash = strrchr(upper_dir_path, '/');
    if (last_slash && last_slash != upper_dir_path) {
        *last_slash = '\0';
        /* mkdir -p equivalent: create each component */
        char tmp[PATH_MAX];
        snprintf(tmp, PATH_MAX, "%s", upper_dir_path);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);  /* Ignore EEXIST */
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }

    /* Open destination for writing, create if not exists */
    int dst_fd = open(upper_path, O_WRONLY | O_CREAT | O_TRUNC, src_stat.st_mode);
    if (dst_fd < 0) {
        close(src_fd);
        return -errno;
    }

    /* Copy contents in chunks */
    char buf[65536];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (write(dst_fd, buf, n) != n) {
            close(src_fd);
            close(dst_fd);
            return -EIO;
        }
    }

    close(src_fd);
    close(dst_fd);

    return 0;
}

/* -----------------------------------------------------------------------
 * HELPER: check if a path exists only in lower_dir (not in upper_dir)
 * Returns 1 if file is lower-only, 0 otherwise.
 * -----------------------------------------------------------------------*/
static int _is_lower_only(const char *path)
{
    struct mini_unionfs_state *state = STATE;

    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    snprintf(upper_path, PATH_MAX, "%s%s", state->upper_dir, path);
    snprintf(lower_path, PATH_MAX, "%s%s", state->lower_dir, path);

    struct stat st;
    int in_upper = (stat(upper_path, &st) == 0);
    int in_lower = (stat(lower_path, &st) == 0);

    return (!in_upper && in_lower);
}

/* -----------------------------------------------------------------------
 * unionfs_open
 *
 * Called when a user opens a file. This is where CoW is triggered.
 *
 * Logic:
 *   1. Resolve the path (respects whiteouts via Member A's resolve_path)
 *   2. If the file doesn't exist, return -ENOENT
 *   3. If the user is opening for WRITE and the file is lower-only:
 *        → Copy it up to upper_dir (CoW!)
 *   4. Open the now-resolved (upper) file and store the fd
 * -----------------------------------------------------------------------*/
static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    char resolved[PATH_MAX];

    /* Step 1: Check if file exists (also checks whiteouts) */
    int res = resolve_path(path, resolved);
    if (res < 0)
        return res;  /* -ENOENT if whited out or doesn't exist */

    /* Step 2: CoW — if user wants to write and file is in lower_dir only */
    int wants_write = (fi->flags & O_WRONLY) || (fi->flags & O_RDWR)
                      || (fi->flags & O_APPEND) || (fi->flags & O_TRUNC);

    if (wants_write && _is_lower_only(path)) {
        int cow_res = _do_copy_up(path);
        if (cow_res < 0)
            return cow_res;

        /* After copy-up, re-resolve — it now lives in upper_dir */
        res = resolve_path(path, resolved);
        if (res < 0)
            return res;
    }

    /* Step 3: Open the actual file and store fd in fi->fh for read/write */
    int fd = open(resolved, fi->flags);
    if (fd < 0)
        return -errno;

    fi->fh = fd;
    return 0;
}

/* -----------------------------------------------------------------------
 * unionfs_read
 *
 * Called when a user reads from an open file.
 * The fd was already stored in fi->fh by unionfs_open.
 *
 * Logic:
 *   - Seek to offset, read `size` bytes into buf
 *   - Return number of bytes actually read
 * -----------------------------------------------------------------------*/
static int unionfs_read(const char *path, char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi)
{
    (void) path;  /* We use fi->fh, path not needed */

    int res = pread(fi->fh, buf, size, offset);
    if (res < 0)
        return -errno;

    return res;
}

/* -----------------------------------------------------------------------
 * unionfs_write
 *
 * Called when a user writes to an open file.
 * By the time we get here, open() has already done CoW if needed.
 * So we can always write — the fd in fi->fh already points to upper_dir.
 *
 * Logic:
 *   - Write `size` bytes from buf at offset
 *   - Return number of bytes actually written
 * -----------------------------------------------------------------------*/
static int unionfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    (void) path;  /* We use fi->fh */

    int res = pwrite(fi->fh, buf, size, offset);
    if (res < 0)
        return -errno;

    return res;
}

/* -----------------------------------------------------------------------
 * unionfs_create
 *
 * Called when a user creates a new file (e.g. touch newfile.txt).
 * New files always go into upper_dir — never lower_dir.
 *
 * Logic:
 *   1. Build upper_dir path
 *   2. Create the file with the given mode
 *   3. Store fd in fi->fh
 * -----------------------------------------------------------------------*/
static int unionfs_create(const char *path, mode_t mode,
                          struct fuse_file_info *fi)
{
    struct mini_unionfs_state *state = STATE;

    char upper_path[PATH_MAX];
    snprintf(upper_path, PATH_MAX, "%s%s", state->upper_dir, path);

    /*
     * Ensure parent dirs exist in upper_dir.
     * e.g. if creating /subdir/newfile.txt, upper_dir/subdir/ must exist.
     */
    char dir_path[PATH_MAX];
    snprintf(dir_path, PATH_MAX, "%s", upper_path);
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash && last_slash != dir_path) {
        *last_slash = '\0';
        char tmp[PATH_MAX];
        snprintf(tmp, PATH_MAX, "%s", dir_path);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }

    int fd = open(upper_path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0)
        return -errno;

    fi->fh = fd;
    return 0;
}

/* -----------------------------------------------------------------------
 * unionfs_truncate
 *
 * Called when a file's size is changed (e.g. truncate, or open with O_TRUNC).
 *
 * Logic:
 *   1. Resolve path
 *   2. If file is lower-only, CoW first (can't truncate a read-only layer)
 *   3. Truncate the upper_dir copy to the given size
 * -----------------------------------------------------------------------*/
static int unionfs_truncate(const char *path, off_t size,
                            struct fuse_file_info *fi)
{
    struct mini_unionfs_state *state = STATE;

    char resolved[PATH_MAX];
    int res = resolve_path(path, resolved);
    if (res < 0)
        return res;

    /* CoW if needed before truncating */
    if (_is_lower_only(path)) {
        int cow_res = _do_copy_up(path);
        if (cow_res < 0)
            return cow_res;
    }

    /* Now truncate in upper_dir */
    char upper_path[PATH_MAX];
    snprintf(upper_path, PATH_MAX, "%s%s", state->upper_dir, path);

    if (fi != NULL) {
        res = ftruncate(fi->fh, size);
    } else {
        res = truncate(upper_path, size);
    }

    if (res < 0)
        return -errno;

    return 0;
}

/* -----------------------------------------------------------------------
 * unionfs_release
 *
 * Called when the last file descriptor to an open file is closed.
 * We just close the fd we stored in fi->fh.
 * -----------------------------------------------------------------------*/
static int unionfs_release(const char *path, struct fuse_file_info *fi)
{
    (void) path;
    close(fi->fh);
    return 0;
}

/*
 * -----------------------------------------------------------------------
 * WHAT TO ADD TO fuse_operations IN main.c (Member C's section)
 * -----------------------------------------------------------------------
 * When Member C sets up the fuse_operations struct, add these entries:
 *
 *   static struct fuse_operations unionfs_oper = {
 *       .getattr  = unionfs_getattr,   // Member A
 *       .readdir  = unionfs_readdir,   // Member A
 *       .open     = unionfs_open,      // Member B ← you
 *       .read     = unionfs_read,      // Member B ← you
 *       .write    = unionfs_write,     // Member B ← you
 *       .create   = unionfs_create,    // Member B ← you
 *       .truncate = unionfs_truncate,  // Member B ← you
 *       .release  = unionfs_release,   // Member B ← you
 *       .unlink   = unionfs_unlink,    // Member C
 *       .mkdir    = unionfs_mkdir,     // Member C
 *       .rmdir    = unionfs_rmdir,     // Member C
 *   };
 */
