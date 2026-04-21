# Mini-UnionFS

A simplified Union File System implemented in userspace using FUSE (Filesystem in Userspace), built as part of a Cloud Computing course project.

## What it does

Mini-UnionFS merges two directories into a single unified virtual filesystem — mimicking the core mechanism that powers Docker container layers:

- **lower_dir** — read-only base layer (like a Docker base image)
- **upper_dir** — read-write container layer (your changes go here)
- **mount_dir** — the merged view that users interact with

## Features

- **Layer Stacking** — files from both layers appear merged; upper layer takes precedence
- **Copy-on-Write (CoW)** — modifying a lower-layer file automatically copies it to upper first; lower is never touched
- **Whiteout** — deleting a lower-layer file creates a `.wh.<filename>` marker in upper, hiding it from the merged view
- **Full POSIX support** — `getattr`, `readdir`, `read`, `write`, `create`, `unlink`, `mkdir`, `rmdir`, `truncate`

## How it relates to Docker

When Docker runs a container, it does not copy the entire base image. It stacks a writable layer on top of read-only image layers. Mini-UnionFS is exactly this mechanism — built from scratch.

```
Docker concept          Mini-UnionFS equivalent
──────────────────────────────────────────────
Base image layers   →   lower_dir  (read-only)
Container layer     →   upper_dir  (read-write)
Merged view         →   mount_dir  (what you see)
```

## Team

| Member | Functions |
|--------|-----------|
| A | `resolve_path`, `copy_up`, `getattr`, `readdir` |
| B | `open`, `read`, `write`, `create`, `truncate` |
| C | `unlink`, `mkdir`, `rmdir`, `main()`, `Makefile` |

## Environment

- Ubuntu 22.04 LTS
- FUSE 2 (`libfuse-dev`)
- Language: C

## Setup

```bash
# Install dependencies
sudo apt update
sudo apt install -y libfuse-dev fuse build-essential

# Add yourself to fuse group
sudo usermod -aG fuse $USER
# Log out and back in
```

## Build

```bash
make
```

## Run

```bash
# Create your layer directories
mkdir -p /tmp/lower /tmp/upper /tmp/mnt

# Seed the base layer
echo "hello from base" > /tmp/lower/hello.txt
echo "to be deleted"  > /tmp/lower/delete_me.txt

# Mount
./mini_unionfs /tmp/lower /tmp/upper /tmp/mnt

# Unmount when done
fusermount -u /tmp/mnt
```

## Demo

Open a second terminal after mounting and run:

```bash
# Test 1 — Layer visibility
cat /tmp/mnt/hello.txt          # prints: hello from base

# Test 2 — Copy-on-Write
echo "modified" >> /tmp/mnt/hello.txt
cat /tmp/upper/hello.txt        # modified copy appears here
cat /tmp/lower/hello.txt        # original is UNCHANGED

# Test 3 — Whiteout
rm /tmp/mnt/delete_me.txt
ls /tmp/upper/                  # .wh.delete_me.txt exists
ls /tmp/mnt/                    # delete_me.txt is hidden
cat /tmp/lower/delete_me.txt    # original still in lower
```

## Automated Tests

```bash
make test
# Starting Mini-UnionFS Test Suite...
# Test 1: Layer Visibility...   PASSED
# Test 2: Copy-on-Write...      PASSED
# Test 3: Whiteout mechanism... PASSED
# Test Suite Completed.
```

## Debug mode

```bash
./mini_unionfs /tmp/lower /tmp/upper /tmp/mnt -d
# Prints every FUSE callback as it happens
```

## Project Structure

```
mini_unionfs/
├── mini_unionfs.c    # final combined FUSE implementation
├── memA.c            # Member A: resolve_path, copy_up, getattr, readdir
├── memB.c            # Member B: open, read, write, create, truncate
├── Makefile          # build + test targets
├── test_unionfs.sh   # automated test suite (all 3 features)
├── test_memB.sh      # Member B unit tests
└── README.md
```
