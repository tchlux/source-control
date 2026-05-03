// ___________________________________________________________________
//                        source_control.c
//
// DESCRIPTION
//  A small local source-control tracker. It records reverse patches
//  for explicitly added files so older states can be restored as new
//  commits from the current working tree.
//
// COMMANDS
//   sc init
//   sc add [paths...]
//   sc ignore <paths...>
//   sc rm [paths...]
//   sc status
//   sc ls [--tracked] [--untracked] [--ignored]
//   sc diff [--full] [path|COMMIT] [--against COMMIT]
//   sc commit -m "message"
//   sc amend [-m "message"]
//   sc log [path]
//   sc browse [COMMIT] [FILE]
//   sc clean
//   sc trace <path> [--lines A[:B]] [--regex REGEX] [START..END|--after COMMIT|--before COMMIT]
//   sc squash COMMIT|HEAD|START..END|ALL
//   sc restore COMMIT
//   sc revert [--no-warning] <path>
//   sc import
//   sc destroy
//
// ___________________________________________________________________

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SC_DIR ".source-control"
#define DEFAULT_MATCH "default"
#define BLOCK_SIZE 2048
#define LIST_LIMIT 10
#define STATUS_UNTRACKED_LIMIT 20
#define DIFF_LINE_LIMIT 100
#define DIFF_CONTEXT_LINES 3
#define DIFF_CELL_LIMIT 100000000LL
#define STATUS_CELL_LIMIT 100000000LL
#define PROGRESS_INTERVAL_MS 3000LL
#define MAX_BLOCK_CANDIDATES 128
#define TEXT_SAMPLE_SIZE 8192
#define INITIAL_SIZE 16
#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL
#define SOURCE_FILE 0
#define SOURCE_LINK 1
#define SOURCE_NONE 2
#define STAGE_ADD 1
#define STAGE_MOD 2
#define STAGE_DEL 3
#define STAGE_MOVE 4
#define STATUS_STAGED 1
#define STATUS_UNSTAGED 2
#define IGNORE_LITERAL 0
#define IGNORE_GLOB 1
#define IGNORE_REGEX 2

void match(const char * regex, const char * string, int * start, int * end);

typedef struct {
  int type;
  char * pattern;
} ignore_rule_t;

typedef struct {
  char root[PATH_MAX];
  char sc[PATH_MAX];
  char pattern[4096];
  ignore_rule_t * ignores;
  int nignores;
  int signores;
} repo_t;

typedef struct {
  uint64_t path_hash;
  uint64_t content_hash;
  unsigned long long size;
  long long mtime;
  int type;
  int removed;
  char * path;
} index_entry_t;

typedef struct {
  index_entry_t * v;
  int n;
  int size;
} index_t;

typedef struct {
  uint64_t path_hash;
  uint64_t content_hash;
  unsigned long long size;
  long long mtime;
  int type;
  int action;
  char * old_path;
  char * path;
} stage_entry_t;

typedef struct {
  stage_entry_t * v;
  int n;
  int size;
} stage_t;

typedef struct {
  char ** v;
  int n;
  int size;
} paths_t;

typedef struct {
  unsigned char * data;
  size_t n;
} bytes_t;

typedef struct {
  const char * label;
  const char * unit;
  long long last_ms;
  long long count;
  int shown;
} progress_t;

void empty_bytes(bytes_t * b);
int commit_exists(repo_t * repo, unsigned long long id);
int binary_like(bytes_t * b);
int remove_tree(const char * path);
unsigned long long next_commit_id(repo_t * repo);

typedef struct {
  size_t off;
  size_t new_n;
  size_t old_n;
  unsigned char * old;
} edit_t;

typedef struct {
  uint32_t weak;
  uint64_t strong;
  size_t off;
  int next;
} block_match_t;

typedef struct {
  edit_t * v;
  int n;
  int size;
} edits_t;

typedef struct {
  char * path;
  char * old_path;
  uint64_t content_hash;
  unsigned long long size;
  unsigned long long old_size;
  unsigned long long new_size;
  unsigned long long stored_size;
  int kind;
  int old_type;
  int new_type;
  edits_t edits;
} commit_file_t;

typedef struct {
  unsigned long long id;
  unsigned long long parent;
  long long time;
  char * msg;
  commit_file_t * files;
  int n;
} commit_t;

void commit_free(commit_t * c);
int read_commit(repo_t * repo, unsigned long long id, commit_t * c);

typedef struct {
  unsigned long long * v;
  int n;
  int size;
} ids_t;

typedef struct {
  unsigned char * s;
  size_t n;
} line_t;

int split_lines(bytes_t * b, line_t ** out);
int line_eq(line_t * a, line_t * b);
int line_at_offset(line_t * lines, int n, const unsigned char * base, size_t off);
int browse_text_rows(const char * text, int cols);
int focused_diff_states(repo_t * repo, unsigned long long id, commit_file_t * f, bytes_t * old, int * old_type, int * old_exists, bytes_t * new, int * new_type, int * new_exists);

typedef struct {
  int old_start;
  int old_n;
  int new_start;
  int new_n;
} trace_hunk_t;

typedef struct {
  trace_hunk_t * v;
  int n;
  int size;
} trace_hunks_t;

typedef struct {
  int state;
  const char * action;
  char * path;
  int added;
  int deleted;
  int counts;
} status_row_t;

typedef struct {
  status_row_t * v;
  int n;
  int size;
} status_rows_t;

uint64_t hash_bytes(const unsigned char * s, size_t n) {
  uint64_t h = FNV_OFFSET;
  for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * FNV_PRIME;
  return h;
}

uint64_t hash_string(const char * s) {
  return hash_bytes((const unsigned char*) s, strlen(s));
}

int is_dir(const char * path) {
  struct stat st;
  return (lstat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

int is_file(const char * path) {
  struct stat st;
  return (lstat(path, &st) == 0) && S_ISREG(st.st_mode);
}

int path_exists(const char * path) {
  struct stat st;
  return lstat(path, &st) == 0;
}

int mkdir_one(const char * path) {
  return (mkdir(path, 0755) == 0) || (errno == EEXIST);
}

int mkdir_parents(char * path) {
  for (char * p = path + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (! mkdir_one(path)) return 0;
      *p = '/';
    }
  }
  return 1;
}

int write_bytes(const char * path, const unsigned char * data, size_t n) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char * slash = strrchr(tmp, '/');
  if (slash) {
    *slash = '\0';
    if (! mkdir_parents(tmp)) return 0;
    if (! mkdir_one(tmp)) return 0;
  }
  FILE * fp = fopen(path, "wb");
  if (! fp) return 0;
  int ok = fwrite(data, 1, n, fp) == n;
  fclose(fp);
  return ok;
}

int read_bytes(const char * path, bytes_t * b) {
  FILE * fp = fopen(path, "rb");
  if (! fp) return 0;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  rewind(fp);
  b->n = (n < 0) ? 0 : (size_t) n;
  b->data = malloc(b->n ? b->n : 1);
  if (! b->data) {
    fclose(fp);
    return 0;
  }
  int ok = fread(b->data, 1, b->n, fp) == b->n;
  fclose(fp);
  return ok;
}

int read_source(const char * path, bytes_t * b, int * type) {
  struct stat st;
  if (lstat(path, &st) != 0) return 0;
  if (S_ISLNK(st.st_mode)) {
    b->data = malloc(st.st_size + 1);
    if (! b->data) return 0;
    ssize_t n = readlink(path, (char*) b->data, st.st_size + 1);
    if (n < 0) { free(b->data); b->data = NULL; b->n = 0; return 0; }
    b->n = (size_t) n;
    *type = SOURCE_LINK;
    return 1;
  }
  if (! S_ISREG(st.st_mode)) return 0;
  *type = SOURCE_FILE;
  return read_bytes(path, b);
}

int write_source(const char * path, bytes_t * b, int type) {
  char tmp[PATH_MAX], target[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char * slash = strrchr(tmp, '/');
  if (slash) {
    *slash = '\0';
    if (! mkdir_parents(tmp)) return 0;
    if (! mkdir_one(tmp)) return 0;
  }
  unlink(path);
  if (type == SOURCE_LINK) {
    if (b->n >= sizeof(target)) return 0;
    memcpy(target, b->data, b->n);
    target[b->n] = '\0';
    return symlink(target, path) == 0;
  }
  return write_bytes(path, b->data, b->n);
}

void free_bytes(bytes_t * b) {
  free(b->data);
  b->data = NULL;
  b->n = 0;
}

char * xstrdup(const char * s) {
  char * out = malloc(strlen(s) + 1);
  if (out) strcpy(out, s);
  return out;
}

char * xstrndup(const char * s, size_t n) {
  char * out = malloc(n + 1);
  if (! out) return NULL;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

void path_join(char * out, size_t n, const char * a, const char * b) {
  snprintf(out, n, "%s/%s", a, b);
}

void cache_path(repo_t * repo, const char * rel, char * out) {
  snprintf(out, PATH_MAX, "%s/files/%016llx", repo->sc, (unsigned long long) hash_string(rel));
}

void staged_path(repo_t * repo, const char * rel, char * out) {
  snprintf(out, PATH_MAX, "%s/staged/%016llx", repo->sc, (unsigned long long) hash_string(rel));
}

void normalize_rel(char * path) {
  while (strncmp(path, "./", 2) == 0) memmove(path, path + 2, strlen(path + 2) + 1);
  while (path[0] && path[strlen(path) - 1] == '/') path[strlen(path) - 1] = '\0';
}

int ignore_push(repo_t * repo, int type, const char * pattern) {
  if (repo->nignores == repo->signores) {
    repo->signores = repo->signores ? 2 * repo->signores : INITIAL_SIZE;
    ignore_rule_t * v = realloc(repo->ignores, repo->signores * sizeof(ignore_rule_t));
    if (! v) return 0;
    repo->ignores = v;
  }
  repo->ignores[repo->nignores].type = type;
  repo->ignores[repo->nignores].pattern = xstrdup(pattern);
  return repo->ignores[repo->nignores++].pattern != NULL;
}

void read_config_file(repo_t * repo, const char * path, int read_ignores) {
  FILE * fp = fopen(path, "r");
  if (! fp) return;
  char line[8192];
  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "match ", 6) == 0) {
      snprintf(repo->pattern, sizeof(repo->pattern), "%s", line + 6);
      repo->pattern[strcspn(repo->pattern, "\r\n")] = '\0';
    } else if (read_ignores && strncmp(line, "ignore ", 7) == 0) {
      line[strcspn(line, "\r\n")] = '\0';
      normalize_rel(line + 7);
      if (line[7]) ignore_push(repo, IGNORE_LITERAL, line + 7);
    } else if (read_ignores && strncmp(line, "ignore-glob ", 12) == 0) {
      line[strcspn(line, "\r\n")] = '\0';
      normalize_rel(line + 12);
      if (line[12]) ignore_push(repo, IGNORE_GLOB, line + 12);
    } else if (read_ignores && strncmp(line, "ignore-regex ", 13) == 0) {
      line[strcspn(line, "\r\n")] = '\0';
      if (line[13]) ignore_push(repo, IGNORE_REGEX, line + 13);
    }
  }
  fclose(fp);
}

int find_repo(repo_t * repo) {
  char cwd[PATH_MAX], home[PATH_MAX], dir[PATH_MAX];
  if (! getcwd(cwd, sizeof(cwd))) return 0;
  snprintf(home, sizeof(home), "%s", getenv("HOME") ? getenv("HOME") : "/");
  snprintf(dir, sizeof(dir), "%s", cwd);
  while (1) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, SC_DIR);
    if (is_dir(path)) {
      if (! realpath(dir, repo->root)) snprintf(repo->root, sizeof(repo->root), "%s", dir);
      snprintf(repo->sc, sizeof(repo->sc), "%s/%s", repo->root, SC_DIR);
      snprintf(repo->pattern, sizeof(repo->pattern), "%s", DEFAULT_MATCH);
      repo->ignores = NULL;
      repo->nignores = repo->signores = 0;
      char cfg[PATH_MAX];
      snprintf(cfg, sizeof(cfg), "%s/.source-control.config", home);
      read_config_file(repo, cfg, 0);
      snprintf(cfg, sizeof(cfg), "%s/config", repo->sc);
      read_config_file(repo, cfg, 1);
      return 1;
    }
    if (strcmp(dir, home) == 0 || strcmp(dir, "/") == 0) return 0;
    char * slash = strrchr(dir, '/');
    if (! slash || slash == dir) strcpy(dir, "/");
    else *slash = '\0';
  }
}

int init_repo_at(const char * sc) {
  char path[PATH_MAX];
  if (! mkdir_one(sc)) return 0;
  snprintf(path, sizeof(path), "%s/files", sc); if (! mkdir_one(path)) return 0;
  snprintf(path, sizeof(path), "%s/commits", sc); if (! mkdir_one(path)) return 0;
  snprintf(path, sizeof(path), "%s/staged", sc); if (! mkdir_one(path)) return 0;
  snprintf(path, sizeof(path), "%s/head", sc); FILE * fp = fopen(path, "ab"); if (fp) fclose(fp); else return 0;
  snprintf(path, sizeof(path), "%s/index", sc); fp = fopen(path, "ab"); if (fp) fclose(fp); else return 0;
  snprintf(path, sizeof(path), "%s/stage", sc); fp = fopen(path, "ab"); if (fp) fclose(fp); else return 0;
  snprintf(path, sizeof(path), "%s/config", sc); fp = fopen(path, "ab"); if (fp) fclose(fp); else return 0;
  return 1;
}

int init_repo(void) {
  if (! init_repo_at(SC_DIR)) return 1;
  printf("initialized %s\n", SC_DIR);
  return 0;
}

void index_free(index_t * idx) {
  for (int i = 0; i < idx->n; i++) free(idx->v[i].path);
  free(idx->v);
  idx->v = NULL;
  idx->n = idx->size = 0;
}

int index_push(index_t * idx, index_entry_t e) {
  if (idx->n == idx->size) {
    idx->size = idx->size ? 2 * idx->size : INITIAL_SIZE;
    index_entry_t * v = realloc(idx->v, idx->size * sizeof(index_entry_t));
    if (! v) return 0;
    idx->v = v;
  }
  idx->v[idx->n++] = e;
  return 1;
}

int index_find(index_t * idx, const char * path) {
  for (int i = 0; i < idx->n; i++)
    if (strcmp(idx->v[i].path, path) == 0) return i;
  return -1;
}

int load_index(repo_t * repo, index_t * idx) {
  char path[PATH_MAX];
  path_join(path, sizeof(path), repo->sc, "index");
  FILE * fp = fopen(path, "r");
  if (! fp) return 1;
  char line[PATH_MAX + 256];
  while (fgets(line, sizeof(line), fp)) {
    index_entry_t e;
    int used = 0;
    memset(&e, 0, sizeof(e));
    int got = sscanf(line, "%llx %llx %llu %lld %d %d %n",
      (unsigned long long*) &e.path_hash,
      (unsigned long long*) &e.content_hash,
      &e.size, &e.mtime, &e.type, &e.removed, &used);
    if (got != 6) {
      e.type = SOURCE_FILE;
      got = sscanf(line, "%llx %llx %llu %lld %d %n",
        (unsigned long long*) &e.path_hash,
        (unsigned long long*) &e.content_hash,
        &e.size, &e.mtime, &e.removed, &used);
    }
    if (got >= 5) {
      e.path = xstrdup(line + used);
      if (! e.path) { fclose(fp); return 0; }
      e.path[strcspn(e.path, "\r\n")] = '\0';
      if (! index_push(idx, e)) { fclose(fp); return 0; }
    }
  }
  fclose(fp);
  return 1;
}

int save_index(repo_t * repo, index_t * idx) {
  char path[PATH_MAX];
  path_join(path, sizeof(path), repo->sc, "index");
  FILE * fp = fopen(path, "w");
  if (! fp) return 0;
  for (int i = 0; i < idx->n; i++) {
    index_entry_t * e = &idx->v[i];
    fprintf(fp, "%016llx %016llx %llu %lld %d %d %s\n",
      (unsigned long long) e->path_hash,
      (unsigned long long) e->content_hash,
      e->size, e->mtime, e->type, e->removed, e->path);
  }
  fclose(fp);
  return 1;
}

void stage_free(stage_t * st) {
  for (int i = 0; i < st->n; i++) {
    free(st->v[i].old_path);
    free(st->v[i].path);
  }
  free(st->v);
  st->v = NULL;
  st->n = st->size = 0;
}

int stage_push(stage_t * st, stage_entry_t e) {
  if (st->n == st->size) {
    st->size = st->size ? 2 * st->size : INITIAL_SIZE;
    stage_entry_t * v = realloc(st->v, st->size * sizeof(stage_entry_t));
    if (! v) return 0;
    st->v = v;
  }
  st->v[st->n++] = e;
  return 1;
}

int stage_find(stage_t * st, const char * path) {
  for (int i = 0; i < st->n; i++)
    if (strcmp(st->v[i].path, path) == 0) return i;
  return -1;
}

int stage_find_move_from(stage_t * st, const char * path) {
  for (int i = 0; i < st->n; i++)
    if (st->v[i].action == STAGE_MOVE && st->v[i].old_path && strcmp(st->v[i].old_path, path) == 0) return i;
  return -1;
}

int stage_contains_path(stage_t * st, const char * path) {
  return stage_find(st, path) >= 0 || stage_find_move_from(st, path) >= 0;
}

int load_stage(repo_t * repo, stage_t * st) {
  char path[PATH_MAX];
  path_join(path, sizeof(path), repo->sc, "stage");
  FILE * fp = fopen(path, "r");
  if (! fp) return 1;
  char line[PATH_MAX + 256];
  while (fgets(line, sizeof(line), fp)) {
    stage_entry_t e;
    int used = 0;
    memset(&e, 0, sizeof(e));
    if (sscanf(line, "%llx %llx %llu %lld %d %d %n",
      (unsigned long long*) &e.path_hash,
      (unsigned long long*) &e.content_hash,
      &e.size, &e.mtime, &e.type, &e.action, &used) == 6) {
      char * rest = line + used;
      rest[strcspn(rest, "\r\n")] = '\0';
      if (e.action == STAGE_MOVE) {
        char * tab = strchr(rest, '\t');
        if (! tab) continue;
        e.old_path = xstrndup(rest, (size_t) (tab - rest));
        e.path = xstrdup(tab + 1);
      } else {
        e.path = xstrdup(rest);
      }
      if (! e.path) { fclose(fp); return 0; }
      if (! stage_push(st, e)) { fclose(fp); return 0; }
    }
  }
  fclose(fp);
  return 1;
}

int save_stage(repo_t * repo, stage_t * st) {
  char path[PATH_MAX];
  path_join(path, sizeof(path), repo->sc, "stage");
  FILE * fp = fopen(path, "w");
  if (! fp) return 0;
  for (int i = 0; i < st->n; i++) {
    stage_entry_t * e = &st->v[i];
    fprintf(fp, "%016llx %016llx %llu %lld %d %d ",
      (unsigned long long) e->path_hash,
      (unsigned long long) e->content_hash,
      e->size, e->mtime, e->type, e->action);
    if (e->action == STAGE_MOVE) fprintf(fp, "%s\t%s\n", e->old_path, e->path);
    else fprintf(fp, "%s\n", e->path);
  }
  fclose(fp);
  return 1;
}

int clear_stage(repo_t * repo, stage_t * st) {
  for (int i = 0; i < st->n; i++) {
    char path[PATH_MAX];
    staged_path(repo, st->v[i].path, path);
    unlink(path);
    free(st->v[i].old_path);
    free(st->v[i].path);
  }
  free(st->v);
  st->v = NULL;
  st->n = 0;
  st->size = 0;
  return save_stage(repo, st);
}

int stage_set(stage_t * st, stage_entry_t e) {
  int j = stage_find(st, e.path);
  if (j >= 0) {
    free(st->v[j].old_path);
    free(st->v[j].path);
    st->v[j] = e;
    return 1;
  }
  return stage_push(st, e);
}

void stage_remove_at(stage_t * st, int i) {
  free(st->v[i].old_path);
  free(st->v[i].path);
  memmove(st->v + i, st->v + i + 1, (st->n - i - 1) * sizeof(stage_entry_t));
  st->n--;
}

int read_staged(repo_t * repo, stage_entry_t * e, bytes_t * b, int * type, int * exists) {
  char path[PATH_MAX];
  *type = e->type;
  *exists = e->action != STAGE_DEL;
  if (! *exists) { empty_bytes(b); return b->data != NULL; }
  if (e->action == STAGE_MOVE && e->old_path) {
    cache_path(repo, e->old_path, path);
    if (! read_bytes(path, b)) { empty_bytes(b); return b->data != NULL; }
    return 1;
  }
  staged_path(repo, e->path, path);
  if (! read_bytes(path, b)) { empty_bytes(b); return b->data != NULL; }
  return 1;
}

int paths_push(paths_t * ps, const char * path) {
  if (ps->n == ps->size) {
    ps->size = ps->size ? 2 * ps->size : INITIAL_SIZE;
    char ** v = realloc(ps->v, ps->size * sizeof(char*));
    if (! v) return 0;
    ps->v = v;
  }
  ps->v[ps->n] = xstrdup(path);
  return ps->v[ps->n++] != NULL;
}

void paths_free(paths_t * ps) {
  for (int i = 0; i < ps->n; i++) free(ps->v[i]);
  free(ps->v);
}

int path_in_arg(const char * path, const char * arg) {
  size_t n = strlen(arg);
  return ! n || strcmp(path, arg) == 0 || (strncmp(path, arg, n) == 0 && path[n] == '/');
}

void ids_free(ids_t * ids) {
  free(ids->v);
  ids->v = NULL;
  ids->n = ids->size = 0;
}

int ids_push(ids_t * ids, unsigned long long id) {
  if (ids->n == ids->size) {
    ids->size = ids->size ? 2 * ids->size : INITIAL_SIZE;
    unsigned long long * v = realloc(ids->v, ids->size * sizeof(unsigned long long));
    if (! v) return 0;
    ids->v = v;
  }
  ids->v[ids->n++] = id;
  return 1;
}

int ids_find(ids_t * ids, unsigned long long id) {
  for (int i = 0; i < ids->n; i++)
    if (ids->v[i] == id) return i;
  return -1;
}

int rel_path(repo_t * repo, const char * path, char * out) {
  char full[PATH_MAX], cwd[PATH_MAX];
  if (path[0] == '/') snprintf(full, sizeof(full), "%s", path);
  else {
    if (! getcwd(cwd, sizeof(cwd))) return 0;
    char real_cwd[PATH_MAX];
    if (realpath(cwd, real_cwd)) snprintf(cwd, sizeof(cwd), "%s", real_cwd);
    snprintf(full, sizeof(full), "%s/%s", cwd, path);
  }
  size_t n = strlen(repo->root);
  if (strncmp(full, repo->root, n) != 0) return 0;
  snprintf(out, PATH_MAX, "%s", full[n] == '/' ? full + n + 1 : full + n);
  return out[0] != '\0';
}

int text_like_file(const char * path) {
  unsigned char buf[TEXT_SAMPLE_SIZE];
  FILE * fp = fopen(path, "rb");
  if (! fp) return 0;
  size_t n = fread(buf, 1, sizeof(buf), fp);
  int ok = ! ferror(fp);
  fclose(fp);
  if (! ok) return 0;
  for (size_t i = 0; i < n; i++)
    if (buf[i] == 0 || (buf[i] < 32 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t'))
      return 0;
  return 1;
}

int matches(repo_t * repo, const char * rel, const char * full) {
  int start = -1, end = 0;
  if (strcmp(repo->pattern, DEFAULT_MATCH) == 0) return text_like_file(full);
  match(repo->pattern, rel, &start, &end);
  return start >= 0;
}

int default_skip_dir(const char * name) {
  size_t n = strlen(name);
  return strcmp(name, ".git") == 0 || strcmp(name, SC_DIR) == 0 ||
    strcmp(name, "build") == 0 || strcmp(name, "dist") == 0 ||
    (n >= 4 && strcmp(name + n - 4, ".app") == 0);
}

const char * base_name(const char * path) {
  const char * slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

int has_glob_meta(const char * s) {
  return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

int glob_match(const char * pattern, const char * s) {
  if (! *pattern) return ! *s;
  if (*pattern == '*')
    return glob_match(pattern + 1, s) || (*s && *s != '/' && glob_match(pattern, s + 1));
  if (*pattern == '?') return *s && *s != '/' && glob_match(pattern + 1, s + 1);
  return *pattern == *s && glob_match(pattern + 1, s + 1);
}

int ignore_rule_matches_one(ignore_rule_t * rule, const char * rel) {
  int start = -1, end = 0;
  if (rule->type == IGNORE_GLOB) return glob_match(rule->pattern, rel);
  if (rule->type == IGNORE_REGEX) {
    match(rule->pattern, rel, &start, &end);
    return start >= 0;
  }
  size_t n = strlen(rule->pattern);
  return strcmp(rel, rule->pattern) == 0 || (strncmp(rel, rule->pattern, n) == 0 && rel[n] == '/');
}

int ignore_rule_matches(ignore_rule_t * rule, const char * rel) {
  char path[PATH_MAX];
  if (rule->type == IGNORE_LITERAL) return ignore_rule_matches_one(rule, rel);
  snprintf(path, sizeof(path), "%s", rel);
  while (path[0]) {
    if (ignore_rule_matches_one(rule, path)) return 1;
    char * slash = strrchr(path, '/');
    if (! slash) break;
    *slash = '\0';
  }
  return 0;
}

int is_ignored(repo_t * repo, const char * rel) {
  for (int i = 0; i < repo->nignores; i++) {
    if (ignore_rule_matches(&repo->ignores[i], rel)) return 1;
  }
  return 0;
}

int regex_valid(const char * regex) {
  int start = -1, end = 0;
  match(regex, "", &start, &end);
  return start >= 0 || end >= 0;
}

long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long) tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

void progress_start_unit(progress_t * p, const char * label, const char * unit) {
  p->label = label;
  p->unit = unit;
  p->last_ms = now_ms();
  p->count = 0;
  p->shown = 0;
}

void progress_start(progress_t * p, const char * label) {
  progress_start_unit(p, label, "paths");
}

void progress_tick(progress_t * p, long long total, const char * path) {
  if (! p || ! isatty(STDERR_FILENO)) return;
  p->count++;
  long long now = now_ms();
  if (now - p->last_ms < PROGRESS_INTERVAL_MS) return;
  fprintf(stderr, "%s: %lld", p->label, p->count);
  if (total >= 0) fprintf(stderr, "/%lld", total);
  fprintf(stderr, " %s", p->unit);
  if (path && path[0]) fprintf(stderr, " (%s)", path);
  fprintf(stderr, "\n");
  p->last_ms = now;
  p->shown = 1;
}

void progress_done(progress_t * p) {
  if (p && p->shown && isatty(STDERR_FILENO)) fprintf(stderr, "%s: done (%lld %s)\n", p->label, p->count, p->unit);
}

int scan_path(repo_t * repo, const char * rel, paths_t * out, progress_t * progress) {
  progress_tick(progress, -1, rel);
  if (rel[0] && is_ignored(repo, rel)) return 1;
  char full[PATH_MAX];
  path_join(full, sizeof(full), repo->root, rel);
  struct stat st;
  if (lstat(full, &st) == 0 && S_ISLNK(st.st_mode))
    return strcmp(repo->pattern, DEFAULT_MATCH) == 0 ? 1 : (matches(repo, rel, full) ? paths_push(out, rel) : 1);
  if (is_file(full)) return matches(repo, rel, full) ? paths_push(out, rel) : 1;
  if (! is_dir(full)) return 1;
  if (rel[0] && default_skip_dir(base_name(rel))) return 1;
  DIR * dir = opendir(full);
  if (! dir) return 1;
  struct dirent * de;
  while ((de = readdir(dir))) {
    if (de->d_name[0] == '.' && (! de->d_name[1] || (de->d_name[1] == '.' && ! de->d_name[2]))) continue;
    if (default_skip_dir(de->d_name)) continue;
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
    scan_path(repo, child, out, progress);
  }
  closedir(dir);
  return 1;
}

int scan_ignored(repo_t * repo, const char * rel, paths_t * out, progress_t * progress) {
  progress_tick(progress, -1, rel);
  if (rel[0] && (is_ignored(repo, rel) || default_skip_dir(base_name(rel)))) return paths_push(out, rel);
  char full[PATH_MAX];
  path_join(full, sizeof(full), repo->root, rel);
  if (! is_dir(full)) return 1;
  DIR * dir = opendir(full);
  if (! dir) return 1;
  struct dirent * de;
  while ((de = readdir(dir))) {
    if (de->d_name[0] == '.' && (! de->d_name[1] || (de->d_name[1] == '.' && ! de->d_name[2]))) continue;
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
    scan_ignored(repo, child, out, progress);
  }
  closedir(dir);
  return 1;
}

uint32_t weak_sum(const unsigned char * s, size_t n) {
  uint32_t a = 0, b = 0;
  for (size_t i = 0; i < n; i++) {
    a += s[i];
    b += (uint32_t) (n - i) * s[i];
  }
  return (a & 0xffff) | (b << 16);
}

int edits_push(edits_t * es, size_t off, size_t new_n, const unsigned char * old, size_t old_n) {
  if (new_n == 0 && old_n == 0) return 1;
  if (es->n == es->size) {
    es->size = es->size ? 2 * es->size : INITIAL_SIZE;
    edit_t * v = realloc(es->v, es->size * sizeof(edit_t));
    if (! v) return 0;
    es->v = v;
  }
  edit_t * e = &es->v[es->n++];
  e->off = off;
  e->new_n = new_n;
  e->old_n = old_n;
  e->old = malloc(old_n ? old_n : 1);
  if (! e->old) return 0;
  if (old_n) memcpy(e->old, old, old_n);
  return 1;
}

void edits_free(edits_t * es) {
  for (int i = 0; i < es->n; i++) free(es->v[i].old);
  free(es->v);
  es->v = NULL;
  es->n = es->size = 0;
}

int rolling_diff(bytes_t * old, bytes_t * new, edits_t * edits) {
  size_t pre = 0, suf = 0;
  while (pre < old->n && pre < new->n && old->data[pre] == new->data[pre]) pre++;
  while (suf + pre < old->n && suf + pre < new->n &&
         old->data[old->n - 1 - suf] == new->data[new->n - 1 - suf]) suf++;
  size_t old_hi = old->n - suf, new_hi = new->n - suf;
  if (pre == old_hi && pre == new_hi) return 1;
  if (old_hi - pre < BLOCK_SIZE || new_hi - pre < BLOCK_SIZE)
    return edits_push(edits, pre, new_hi - pre, old->data + pre, old_hi - pre);

  int blocks = (int) ((old_hi - pre) / BLOCK_SIZE);
  int nbuckets = 1;
  while (nbuckets < blocks * 2) nbuckets *= 2;
  int * buckets = malloc(nbuckets * sizeof(int));
  block_match_t * matches = malloc(blocks * sizeof(block_match_t));
  if (! buckets || ! matches) {
    free(buckets);
    free(matches);
    return edits_push(edits, pre, new_hi - pre, old->data + pre, old_hi - pre);
  }
  for (int i = 0; i < nbuckets; i++) buckets[i] = -1;
  for (int i = 0; i < blocks; i++) {
    size_t off = pre + (size_t) i * BLOCK_SIZE;
    matches[i].weak = weak_sum(old->data + off, BLOCK_SIZE);
    matches[i].strong = hash_bytes(old->data + off, BLOCK_SIZE);
    matches[i].off = off;
    int b = (int) (matches[i].weak & (uint32_t) (nbuckets - 1));
    matches[i].next = buckets[b];
    buckets[b] = i;
  }

  size_t old_pos = pre, new_pos = pre, i = pre;
  uint32_t w = weak_sum(new->data + i, BLOCK_SIZE);
  while (i + BLOCK_SIZE <= new_hi) {
    int b = (int) (w & (uint32_t) (nbuckets - 1));
    int checked = 0, have_strong = 0;
    uint64_t strong = 0;
    size_t best = (size_t) -1;
    for (int j = buckets[b]; j >= 0; j = matches[j].next) {
      if (++checked > MAX_BLOCK_CANDIDATES) {
        free(buckets);
        free(matches);
        return edits_push(edits, new_pos, new_hi - new_pos, old->data + old_pos, old_hi - old_pos);
      }
      if (matches[j].weak != w || matches[j].off < old_pos) continue;
      if (! have_strong) {
        strong = hash_bytes(new->data + i, BLOCK_SIZE);
        have_strong = 1;
      }
      if (matches[j].strong == strong &&
          memcmp(old->data + matches[j].off, new->data + i, BLOCK_SIZE) == 0 &&
          matches[j].off < best) best = matches[j].off;
    }
    if (best != (size_t) -1) {
      if (! edits_push(edits, new_pos, i - new_pos, old->data + old_pos, best - old_pos)) {
        free(buckets);
        free(matches);
        return 0;
      }
      old_pos = best + BLOCK_SIZE;
      new_pos = i + BLOCK_SIZE;
      i = new_pos;
      if (i + BLOCK_SIZE <= new_hi) w = weak_sum(new->data + i, BLOCK_SIZE);
      continue;
    }
    if (i + BLOCK_SIZE >= new_hi) break;
    uint32_t a = (w & 0xffff) - new->data[i] + new->data[i + BLOCK_SIZE];
    uint32_t bb = (w >> 16) - (uint32_t) BLOCK_SIZE * new->data[i] + a;
    w = (a & 0xffff) | (bb << 16);
    i++;
  }
  int ok = edits_push(edits, new_pos, new_hi - new_pos, old->data + old_pos, old_hi - old_pos);
  free(buckets);
  free(matches);
  return ok;
}

int build_reverse_edits(int kind, bytes_t * old, bytes_t * new, edits_t * edits) {
  if (kind == 1) return edits_push(edits, 0, new->n, NULL, 0);
  if (kind == 2) return edits_push(edits, 0, 0, old->data, old->n);
  return rolling_diff(old, new, edits);
}

int apply_edits(bytes_t * cur, edits_t * edits, bytes_t * out) {
  bytes_t work = {malloc(cur->n ? cur->n : 1), cur->n};
  if (! work.data) return 0;
  memcpy(work.data, cur->data, cur->n);
  for (int i = edits->n - 1; i >= 0; i--) {
    edit_t * e = &edits->v[i];
    size_t n = work.n - e->new_n + e->old_n;
    unsigned char * data = malloc(n ? n : 1);
    if (! data) { free_bytes(&work); return 0; }
    memcpy(data, work.data, e->off);
    memcpy(data + e->off, e->old, e->old_n);
    memcpy(data + e->off + e->old_n, work.data + e->off + e->new_n, work.n - e->off - e->new_n);
    free(work.data);
    work.data = data;
    work.n = n;
  }
  *out = work;
  return 1;
}

void empty_bytes(bytes_t * b) {
  b->data = malloc(1);
  b->n = 0;
}

int bytes_equal(bytes_t * a, int at, int ae, bytes_t * b, int bt, int be) {
  return ae == be && at == bt && a->n == b->n && memcmp(a->data, b->data, a->n) == 0;
}

int bytes_copy(bytes_t * dst, bytes_t * src) {
  dst->data = malloc(src->n ? src->n : 1);
  if (! dst->data) return 0;
  memcpy(dst->data, src->data, src->n);
  dst->n = src->n;
  return 1;
}

unsigned long long read_head(repo_t * repo) {
  char path[PATH_MAX];
  path_join(path, sizeof(path), repo->sc, "head");
  FILE * fp = fopen(path, "r");
  unsigned long long h = 0;
  if (fp) {
    fscanf(fp, "%llu", &h);
    fclose(fp);
  }
  return h;
}

int read_head_id(repo_t * repo, unsigned long long * h) {
  char path[PATH_MAX];
  path_join(path, sizeof(path), repo->sc, "head");
  FILE * fp = fopen(path, "r");
  int ok = fp && fscanf(fp, "%llu", h) == 1;
  if (fp) fclose(fp);
  return ok;
}

int write_head(repo_t * repo, unsigned long long h) {
  char path[PATH_MAX];
  path_join(path, sizeof(path), repo->sc, "head");
  FILE * fp = fopen(path, "w");
  if (! fp) return 0;
  if (h) fprintf(fp, "%llu\n", h);
  fclose(fp);
  return 1;
}

int write_head_id(repo_t * repo, unsigned long long h) {
  char path[PATH_MAX];
  path_join(path, sizeof(path), repo->sc, "head");
  FILE * fp = fopen(path, "w");
  if (! fp) return 0;
  fprintf(fp, "%llu\n", h);
  fclose(fp);
  return 1;
}

int command_add(repo_t * repo, int argc, char ** argv) {
  index_t idx = {0};
  stage_t stage = {0};
  paths_t paths = {0};
  progress_t progress;
  progress_start(&progress, "scan");
  if (! load_index(repo, &idx)) return 1;
  if (! load_stage(repo, &stage)) return 1;
  if (argc == 0) scan_path(repo, "", &paths, &progress);
  for (int i = 0; i < argc; i++) {
    char rel[PATH_MAX], full[PATH_MAX];
    struct stat st;
    if (! rel_path(repo, argv[i], rel)) continue;
    normalize_rel(rel);
    if (strcmp(rel, ".") == 0) rel[0] = '\0';
    if (rel[0] && is_ignored(repo, rel)) continue;
    path_join(full, sizeof(full), repo->root, rel);
    if (lstat(full, &st) == 0 && (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) paths_push(&paths, rel);
    else scan_path(repo, rel, &paths, &progress);
  }
  progress_done(&progress);
  for (int i = 0; i < paths.n; i++) {
    char full[PATH_MAX];
    struct stat fst;
    bytes_t b = {0};
    int type = SOURCE_NONE;
    path_join(full, sizeof(full), repo->root, paths.v[i]);
    if (! read_source(full, &b, &type) || lstat(full, &fst) != 0) continue;
    int j = index_find(&idx, paths.v[i]);
    if (j < 0) {
      index_entry_t e = {hash_string(paths.v[i]), 0, 0, 0, SOURCE_NONE, 0, xstrdup(paths.v[i])};
      index_push(&idx, e);
    } else idx.v[j].removed = 0;
    char cache[PATH_MAX], spath[PATH_MAX];
    cache_path(repo, paths.v[i], cache);
    staged_path(repo, paths.v[i], spath);
    stage_entry_t se = {hash_string(paths.v[i]), hash_bytes(b.data, b.n), b.n, fst.st_mtime, type, is_file(cache) ? STAGE_MOD : STAGE_ADD, NULL, xstrdup(paths.v[i])};
    bytes_t base = {0};
    if (read_bytes(cache, &base) && idx.v[index_find(&idx, paths.v[i])].type == type && base.n == b.n && memcmp(base.data, b.data, b.n) == 0) {
      int si = stage_find(&stage, paths.v[i]);
      if (si >= 0) stage_remove_at(&stage, si);
      unlink(spath);
      free(se.path);
    } else {
      write_bytes(spath, b.data, b.n);
      stage_set(&stage, se);
    }
    free_bytes(&base);
    free_bytes(&b);
    printf("add %s\n", paths.v[i]);
  }
  int ok = save_index(repo, &idx);
  ok = ok && save_stage(repo, &stage);
  paths_free(&paths);
  stage_free(&stage);
  index_free(&idx);
  return ok ? 0 : 1;
}

int command_ignore(repo_t * repo, int argc, char ** argv) {
  if (argc < 1) { fprintf(stderr, "usage: sc ignore [--literal|--glob|--regex] <paths...>\n"); return 1; }
  int mode = -1, paths = 0;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--literal") == 0) { mode = IGNORE_LITERAL; continue; }
    if (strcmp(argv[i], "--glob") == 0) { mode = IGNORE_GLOB; continue; }
    if (strcmp(argv[i], "--regex") == 0) { mode = IGNORE_REGEX; continue; }
    paths++;
    int type = mode >= 0 ? mode : (has_glob_meta(argv[i]) ? IGNORE_GLOB : IGNORE_LITERAL);
    if (type == IGNORE_REGEX && ! regex_valid(argv[i])) { fprintf(stderr, "invalid regex: %s\n", argv[i]); return 1; }
  }
  if (! paths) { fprintf(stderr, "usage: sc ignore [--literal|--glob|--regex] <paths...>\n"); return 1; }

  char cfg[PATH_MAX];
  path_join(cfg, sizeof(cfg), repo->sc, "config");
  FILE * fp = fopen(cfg, "a");
  if (! fp) return 1;
  int ok = 1;
  mode = -1;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--literal") == 0) { mode = IGNORE_LITERAL; continue; }
    if (strcmp(argv[i], "--glob") == 0) { mode = IGNORE_GLOB; continue; }
    if (strcmp(argv[i], "--regex") == 0) { mode = IGNORE_REGEX; continue; }
    int type = mode >= 0 ? mode : (has_glob_meta(argv[i]) ? IGNORE_GLOB : IGNORE_LITERAL);
    char rel[PATH_MAX];
    if (type == IGNORE_REGEX) {
      snprintf(rel, sizeof(rel), "%s", argv[i]);
    } else if (! rel_path(repo, argv[i], rel)) {
      if (argv[i][0] == '/') { fprintf(stderr, "outside repository: %s\n", argv[i]); ok = 0; continue; }
      snprintf(rel, sizeof(rel), "%s", argv[i]);
    }
    if (type != IGNORE_REGEX) normalize_rel(rel);
    if (! rel[0]) continue;
    if (is_ignored(repo, rel)) continue;
    const char * key = type == IGNORE_GLOB ? "ignore-glob" : type == IGNORE_REGEX ? "ignore-regex" : "ignore";
    fprintf(fp, "%s %s\n", key, rel);
    ignore_push(repo, type, rel);
    printf("%s %s\n", key, rel);
  }
  fclose(fp);
  return ok ? 0 : 1;
}

int command_rm(repo_t * repo, int argc, char ** argv) {
  index_t idx = {0};
  stage_t stage = {0};
  paths_t targets = {0};
  if (! load_index(repo, &idx)) return 1;
  if (! load_stage(repo, &stage)) return 1;
  int ok = 1;
  for (int i = 0; ok && i < argc; i++) {
    char rel[PATH_MAX];
    if (! rel_path(repo, argv[i], rel)) snprintf(rel, sizeof(rel), "%s", argv[i]);
    normalize_rel(rel);
    if (strcmp(rel, ".") == 0) rel[0] = '\0';
    for (int j = 0; j < idx.n; j++) {
      if (! path_in_arg(idx.v[j].path, rel)) continue;
      int seen = 0;
      for (int k = 0; k < targets.n; k++) seen = seen || strcmp(targets.v[k], idx.v[j].path) == 0;
      if (! seen && ! paths_push(&targets, idx.v[j].path)) { ok = 0; break; }
    }
  }
  for (int i = 0; ok && i < targets.n; i++) {
    char * rel = targets.v[i];
    int j = index_find(&idx, rel);
    if (j >= 0) {
      char full[PATH_MAX], cache[PATH_MAX], spath[PATH_MAX];
      path_join(full, sizeof(full), repo->root, rel);
      cache_path(repo, rel, cache);
      staged_path(repo, rel, spath);
      unlink(full);
      unlink(spath);
      if (! is_file(cache)) {
        int si = stage_find(&stage, rel);
        if (si >= 0) stage_remove_at(&stage, si);
        free(idx.v[j].path);
        memmove(idx.v + j, idx.v + j + 1, (idx.n - j - 1) * sizeof(index_entry_t));
        idx.n--;
      } else {
        stage_entry_t se = {hash_string(rel), 0, 0, 0, SOURCE_NONE, STAGE_DEL, NULL, xstrdup(rel)};
        stage_set(&stage, se);
      }
      printf("rm %s\n", rel);
    }
  }
  ok = ok && save_index(repo, &idx);
  ok = ok && save_stage(repo, &stage);
  paths_free(&targets);
  stage_free(&stage);
  index_free(&idx);
  return ok ? 0 : 1;
}

int command_mv(repo_t * repo, int argc, char ** argv) {
  if (argc != 2) { fprintf(stderr, "usage: sc mv <old> <new>\n"); return 1; }
  index_t idx = {0};
  stage_t stage = {0};
  if (! load_index(repo, &idx)) return 1;
  if (! load_stage(repo, &stage)) return 1;
  char old_rel[PATH_MAX], new_rel[PATH_MAX], old_full[PATH_MAX], new_full[PATH_MAX];
  int rc = 1;
  if (! rel_path(repo, argv[0], old_rel) || ! rel_path(repo, argv[1], new_rel)) {
    fprintf(stderr, "outside repository\n");
    goto done;
  }
  normalize_rel(old_rel);
  normalize_rel(new_rel);
  int old_i = index_find(&idx, old_rel);
  if (old_i < 0) { fprintf(stderr, "not tracked: %s\n", old_rel); goto done; }
  if (index_find(&idx, new_rel) >= 0) { fprintf(stderr, "already tracked: %s\n", new_rel); goto done; }
  if (stage_find(&stage, old_rel) >= 0 || stage_find(&stage, new_rel) >= 0) {
    fprintf(stderr, "path already staged\n");
    goto done;
  }
  path_join(old_full, sizeof(old_full), repo->root, old_rel);
  path_join(new_full, sizeof(new_full), repo->root, new_rel);
  if (lstat(new_full, &(struct stat){0}) == 0) { fprintf(stderr, "destination exists: %s\n", new_rel); goto done; }
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", new_full);
  char * slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    if (! mkdir_parents(dir) || ! mkdir_one(dir)) { perror("mv"); goto done; }
  }
  if (rename(old_full, new_full) != 0) { perror("mv"); goto done; }
  struct stat st;
  if (lstat(new_full, &st) != 0) goto done;
  stage_entry_t se = {
    hash_string(new_rel),
    idx.v[old_i].content_hash,
    idx.v[old_i].size,
    st.st_mtime,
    idx.v[old_i].type,
    STAGE_MOVE,
    xstrdup(old_rel),
    xstrdup(new_rel)
  };
  if (! se.old_path || ! se.path || ! stage_set(&stage, se)) {
    free(se.old_path);
    free(se.path);
    goto done;
  }
  if (! save_stage(repo, &stage)) goto done;
  printf("mv %s %s\n", old_rel, new_rel);
  rc = 0;
done:
  stage_free(&stage);
  index_free(&idx);
  return rc;
}

int command_revert(repo_t * repo, int argc, char ** argv) {
  int warn = 1;
  char * arg = NULL;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--no-warning") == 0) warn = 0;
    else if (argv[i][0] == '-') { fprintf(stderr, "usage: sc revert [--no-warning] <path>\n"); return 1; }
    else if (! arg) arg = argv[i];
    else { fprintf(stderr, "usage: sc revert [--no-warning] <path>\n"); return 1; }
  }
  if (! arg) { fprintf(stderr, "usage: sc revert [--no-warning] <path>\n"); return 1; }

  index_t idx = {0};
  stage_t stage = {0};
  bytes_t base = {0}, work = {0};
  int rc = 1, base_exists = 0, work_exists = 0, work_type = SOURCE_NONE;
  char rel[PATH_MAX], full[PATH_MAX], cache[PATH_MAX], spath[PATH_MAX];
  if (! rel_path(repo, arg, rel)) { fprintf(stderr, "outside repository: %s\n", arg); return 1; }
  normalize_rel(rel);
  if (! load_index(repo, &idx) || ! load_stage(repo, &stage)) goto done;
  int j = index_find(&idx, rel), si = stage_find(&stage, rel);
  if (j < 0 && si < 0) { fprintf(stderr, "not tracked: %s\n", rel); goto done; }

  path_join(full, sizeof(full), repo->root, rel);
  cache_path(repo, rel, cache);
  staged_path(repo, rel, spath);
  base_exists = read_bytes(cache, &base);
  if (! base_exists) empty_bytes(&base);
  if (read_source(full, &work, &work_type)) work_exists = 1;
  else empty_bytes(&work);
  int base_type = (j >= 0 && base_exists) ? idx.v[j].type : SOURCE_NONE;
  if (si < 0 && bytes_equal(&base, base_type, base_exists, &work, work_type, work_exists)) {
    printf("nothing to revert\n");
    rc = 0;
    goto done;
  }

  if (warn) {
    char answer[64];
    printf("This cannot be undone. Proceed? [Y/n] ");
    fflush(stdout);
    if (! fgets(answer, sizeof(answer), stdin)) answer[0] = 'n';
    if (answer[0] == 'n' || answer[0] == 'N') { printf("revert cancelled\n"); goto done; }
  }

  if (base_exists) {
    struct stat st;
    if (! write_source(full, &base, base_type) || lstat(full, &st) != 0) goto done;
    if (j >= 0) {
      idx.v[j].content_hash = hash_bytes(base.data, base.n);
      idx.v[j].size = base.n;
      idx.v[j].mtime = st.st_mtime;
      idx.v[j].type = base_type;
      idx.v[j].removed = 0;
    }
  } else {
    unlink(full);
    if (j >= 0) {
      free(idx.v[j].path);
      memmove(idx.v + j, idx.v + j + 1, (idx.n - j - 1) * sizeof(index_entry_t));
      idx.n--;
    }
  }
  if (si >= 0) stage_remove_at(&stage, si);
  unlink(spath);
  if (! save_index(repo, &idx) || ! save_stage(repo, &stage)) goto done;
  printf("revert %s\n", rel);
  rc = 0;
done:
  free_bytes(&base);
  free_bytes(&work);
  stage_free(&stage);
  index_free(&idx);
  return rc;
}

int cmp_path_ptrs(const void * a, const void * b) {
  return strcmp(*(char**) a, *(char**) b);
}

int cmp_index_entries(const void * a, const void * b) {
  index_entry_t * x = (index_entry_t*) a;
  index_entry_t * y = (index_entry_t*) b;
  if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
  return strcmp(x->path, y->path);
}

int list_untracked(repo_t * repo, index_t * idx, stage_t * stage, int limit) {
  paths_t paths = {0};
  progress_t progress;
  progress_start(&progress, "scan");
  scan_path(repo, "", &paths, &progress);
  progress_done(&progress);
  qsort(paths.v, paths.n, sizeof(char*), cmp_path_ptrs);
  int shown = 0, total = 0;
  for (int i = 0; i < paths.n; i++) {
    if (index_find(idx, paths.v[i]) >= 0) continue;
    if (stage && stage_contains_path(stage, paths.v[i])) continue;
    total++;
    if (limit < 0 || shown < limit) {
      printf("untracked: %s\n", paths.v[i]);
      shown++;
    }
  }
  if (limit >= 0 && total > limit) {
    printf("... + %d more untracked\n", total - limit);
    printf("use: sc ls --untracked\n");
  }
  paths_free(&paths);
  return total;
}

int list_ignored(repo_t * repo) {
  paths_t paths = {0};
  progress_t progress;
  progress_start(&progress, "scan");
  scan_ignored(repo, "", &paths, &progress);
  progress_done(&progress);
  qsort(paths.v, paths.n, sizeof(char*), cmp_path_ptrs);
  for (int i = 0; i < paths.n; i++) printf("ignored: %s\n", paths.v[i]);
  int total = paths.n;
  paths_free(&paths);
  return total;
}

void status_rows_free(status_rows_t * rows) {
  for (int i = 0; i < rows->n; i++) free(rows->v[i].path);
  free(rows->v);
  rows->v = NULL;
  rows->n = rows->size = 0;
}

int status_push(status_rows_t * rows, int state, const char * action, const char * path, int added, int deleted, int counts) {
  if (rows->n == rows->size) {
    rows->size = rows->size ? 2 * rows->size : INITIAL_SIZE;
    status_row_t * v = realloc(rows->v, rows->size * sizeof(status_row_t));
    if (! v) return 0;
    rows->v = v;
  }
  char * copy = xstrdup(path);
  if (! copy) return 0;
  rows->v[rows->n++] = (status_row_t){state, action, copy, added, deleted, counts};
  return 1;
}

int text_line_count(bytes_t * b) {
  if (binary_like(b)) return -1;
  if (! b->n) return 0;
  int n = 0;
  for (size_t i = 0; i < b->n; i++) n += b->data[i] == '\n';
  return n + (b->data[b->n - 1] != '\n');
}

int status_line_counts(bytes_t * old, int old_type, int old_exists, bytes_t * new, int new_type, int new_exists, int * added, int * deleted) {
  *added = *deleted = 0;
  if ((old_exists && old_type != SOURCE_FILE) || (new_exists && new_type != SOURCE_FILE)) return 0;
  if (! old_exists && new_exists) { *added = text_line_count(new); return *added >= 0; }
  if (old_exists && ! new_exists) { *deleted = text_line_count(old); return *deleted >= 0; }
  if (! old_exists || ! new_exists || binary_like(old) || binary_like(new)) return 0;

  line_t * a = NULL, * b = NULL;
  int an = split_lines(old, &a), bn = split_lines(new, &b);
  if (an < 0 || bn < 0) { free(a); free(b); return 0; }
  int pre = 0, suf = 0;
  while (pre < an && pre < bn && line_eq(&a[pre], &b[pre])) pre++;
  while (suf + pre < an && suf + pre < bn && line_eq(&a[an - 1 - suf], &b[bn - 1 - suf])) suf++;
  int ao = pre, bo = pre;
  an -= pre + suf;
  bn -= pre + suf;
  if ((long long) (an + 1) * (long long) (bn + 1) > STATUS_CELL_LIMIT) { free(a); free(b); return 0; }

  int cols = bn + 1;
  int * dp = calloc((size_t) (an + 1) * (size_t) (bn + 1), sizeof(int));
  if (! dp) { free(a); free(b); return 0; }
  for (int i = an - 1; i >= 0; i--)
    for (int j = bn - 1; j >= 0; j--)
      dp[i * cols + j] = line_eq(&a[ao + i], &b[bo + j]) ? 1 + dp[(i + 1) * cols + j + 1] :
        (dp[(i + 1) * cols + j] > dp[i * cols + j + 1] ? dp[(i + 1) * cols + j] : dp[i * cols + j + 1]);
  *deleted = an - dp[0];
  *added = bn - dp[0];
  free(dp);
  free(a);
  free(b);
  return 1;
}

void status_push_change(status_rows_t * rows, int state, const char * action, const char * path, bytes_t * old, int old_type, int old_exists, bytes_t * new, int new_type, int new_exists) {
  int added = 0, deleted = 0;
  int counts = status_line_counts(old, old_type, old_exists, new, new_type, new_exists, &added, &deleted);
  status_push(rows, state, action, path, added, deleted, counts);
}

int collect_untracked(repo_t * repo, index_t * idx, stage_t * stage, int limit, paths_t * shown, int * more) {
  paths_t paths = {0};
  progress_t progress;
  progress_start(&progress, "scan");
  scan_path(repo, "", &paths, &progress);
  progress_done(&progress);
  qsort(paths.v, paths.n, sizeof(char*), cmp_path_ptrs);
  int total = 0;
  for (int i = 0; i < paths.n; i++) {
    if (index_find(idx, paths.v[i]) >= 0) continue;
    if (stage_contains_path(stage, paths.v[i])) continue;
    if (limit < 0 || total < limit) paths_push(shown, paths.v[i]);
    total++;
  }
  *more = (limit >= 0 && total > limit) ? total - limit : 0;
  paths_free(&paths);
  return total;
}

int collect_untracked_from_paths(index_t * idx, stage_t * stage, paths_t * paths, int * skip, int limit, paths_t * shown, int * more) {
  int total = 0;
  for (int i = 0; i < paths->n; i++) {
    if (skip && skip[i]) continue;
    if (index_find(idx, paths->v[i]) >= 0) continue;
    if (stage_contains_path(stage, paths->v[i])) continue;
    if (limit < 0 || total < limit) paths_push(shown, paths->v[i]);
    total++;
  }
  *more = (limit >= 0 && total > limit) ? total - limit : 0;
  return total;
}

const char * status_row_color(status_row_t * r, const char * green, const char * blue, const char * purple, const char * red) {
  if (strcmp(r->action, "deleted") == 0) return red;
  if (strcmp(r->action, "modified") == 0) return green;
  if (strcmp(r->action, "type-change") == 0) return purple;
  if (r->state == STATUS_STAGED && strcmp(r->action, "added") == 0) return blue;
  return "";
}

void print_status_counts(status_row_t * r, const char * green, const char * red, const char * reset) {
  if (! r->counts) return;
  printf("  (");
  if (r->added) printf("%s+%d%s", green, r->added, reset);
  if (r->added && r->deleted) printf("/");
  if (r->deleted) printf("%s-%d%s", red, r->deleted, reset);
  printf(")");
}

void print_status_section(status_rows_t * rows, int state, const char * title, const char * green, const char * blue, const char * purple, const char * red, const char * reset, int * printed) {
  int any = 0;
  int path_width = 0;
  for (int i = 0; i < rows->n; i++) {
    any |= rows->v[i].state == state;
    if (rows->v[i].state == state && (int) strlen(rows->v[i].path) > path_width)
      path_width = (int) strlen(rows->v[i].path);
  }
  if (! any) return;
  if (*printed) printf("\n");
  printf("%s\n", title);
  for (int i = 0; i < rows->n; i++) {
    status_row_t * r = &rows->v[i];
    if (r->state != state) continue;
    const char * color = status_row_color(r, green, blue, purple, red);
    printf("  %s%-11s %-*s%s", color, r->action, path_width, r->path, reset);
    print_status_counts(r, green, red, reset);
    printf("\n");
  }
  *printed = 1;
}

void print_status(status_rows_t * rows, paths_t * untracked, int more) {
  int color = isatty(STDOUT_FILENO);
  const char * green = color ? "\033[32m" : "";
  const char * blue = color ? "\033[34m" : "";
  const char * purple = color ? "\033[35m" : "";
  const char * orange = color ? "\033[38;5;208m" : "";
  const char * red = color ? "\033[31m" : "";
  const char * dim = color ? "\033[2m" : "";
  const char * reset = color ? "\033[0m" : "";
  int printed = 0;
  print_status_section(rows, STATUS_STAGED, "Staged", green, blue, purple, red, reset, &printed);
  print_status_section(rows, STATUS_UNSTAGED, "Unstaged", green, blue, purple, red, reset, &printed);
  if (untracked->n || more) {
    if (printed) printf("\n");
    printf("Untracked\n");
    for (int i = 0; i < untracked->n; i++) printf("  %s%s%s\n", orange, untracked->v[i], reset);
    if (more) {
      printf("  %s... %d more untracked%s\n", dim, more, reset);
      printf("  %suse: sc ls --untracked%s\n", dim, reset);
    }
  }
}

int print_status_head(repo_t * repo) {
  unsigned long long head = 0;
  commit_t c = {0};
  if (! read_head_id(repo, &head) || ! read_commit(repo, head, &c)) return 0;
  time_t tt = (time_t) c.time;
  char * ts = ctime(&tt);
  if (ts) ts[strcspn(ts, "\n")] = '\0';
  if (c.msg && ! strchr(c.msg, '\n')) printf("%llu %s %s\n", head, ts ? ts : "unknown time", c.msg);
  else printf("%llu %s\n", head, ts ? ts : "unknown time");
  commit_free(&c);
  return 1;
}

int stage_unstaged(repo_t * repo, index_t * idx, stage_t * stage) {
  int n = 0;
  progress_t progress;
  progress_start(&progress, "stage");
  for (int i = 0; i < idx->n; i++) {
    index_entry_t * e = &idx->v[i];
    progress_tick(&progress, idx->n, e->path);
    bytes_t base = {0}, work = {0};
    int base_exists = 0, work_exists = 0, work_type = SOURCE_NONE;
    char full[PATH_MAX], cache[PATH_MAX];
    path_join(full, sizeof(full), repo->root, e->path);
    cache_path(repo, e->path, cache);
    base_exists = read_bytes(cache, &base);
    if (! base_exists) empty_bytes(&base);
    if (read_source(full, &work, &work_type)) work_exists = 1;
    else empty_bytes(&work);
    if (! bytes_equal(&base, e->type, base_exists, &work, work_type, work_exists)) {
      char spath[PATH_MAX];
      staged_path(repo, e->path, spath);
      stage_entry_t se = {e->path_hash, work_exists ? hash_bytes(work.data, work.n) : 0, work_exists ? work.n : 0, e->mtime, work_type, work_exists ? STAGE_MOD : STAGE_DEL, NULL, xstrdup(e->path)};
      if (se.path && (! work_exists || write_bytes(spath, work.data, work.n))) {
        if (! work_exists) unlink(spath);
        if (stage_set(stage, se)) n++;
      } else free(se.path);
    }
    free_bytes(&base);
    free_bytes(&work);
  }
  progress_done(&progress);
  paths_t paths = {0};
  progress_start(&progress, "scan");
  scan_path(repo, "", &paths, &progress);
  progress_done(&progress);
  for (int i = 0; i < paths.n; i++) {
    if (index_find(idx, paths.v[i]) >= 0) continue;
    char full[PATH_MAX], spath[PATH_MAX];
    struct stat st;
    bytes_t b = {0};
    int type = SOURCE_NONE;
    path_join(full, sizeof(full), repo->root, paths.v[i]);
    staged_path(repo, paths.v[i], spath);
    if (! read_source(full, &b, &type) || lstat(full, &st) != 0) continue;
    index_entry_t e = {hash_string(paths.v[i]), 0, 0, 0, SOURCE_NONE, 0, xstrdup(paths.v[i])};
    stage_entry_t se = {e.path_hash, hash_bytes(b.data, b.n), b.n, st.st_mtime, type, STAGE_ADD, NULL, xstrdup(paths.v[i])};
    int staged = e.path && se.path && write_bytes(spath, b.data, b.n) && stage_set(stage, se);
    if (staged && index_push(idx, e)) n++;
    else {
      if (staged) {
        int si = stage_find(stage, paths.v[i]);
        if (si >= 0) stage_remove_at(stage, si);
      }
      free(e.path);
      if (! staged) free(se.path);
    }
    free_bytes(&b);
  }
  paths_free(&paths);
  return n;
}

int pair_staged_moves(index_t * idx, stage_t * stage, int * move_pair, int * skip_stage) {
  for (int i = 0; i < stage->n; i++) move_pair[i] = -1;
  for (int i = 0; i < stage->n; i++) {
    stage_entry_t * del = &stage->v[i];
    if (del->action != STAGE_DEL) continue;
    int old_i = index_find(idx, del->path);
    if (old_i < 0) continue;
    for (int j = 0; j < stage->n; j++) {
      stage_entry_t * add = &stage->v[j];
      if (skip_stage[j] || add->action != STAGE_ADD) continue;
      if (idx->v[old_i].content_hash != add->content_hash ||
          idx->v[old_i].size != add->size ||
          idx->v[old_i].type != add->type) continue;
      move_pair[i] = j;
      skip_stage[j] = 1;
      break;
    }
  }
  return 1;
}

int work_path_matches_index(repo_t * repo, index_entry_t * e, const char * path) {
  char full[PATH_MAX];
  bytes_t b = {0};
  int type = SOURCE_NONE, ok = 0;
  path_join(full, sizeof(full), repo->root, path);
  if (read_source(full, &b, &type))
    ok = type == e->type && b.n == e->size && hash_bytes(b.data, b.n) == e->content_hash;
  free_bytes(&b);
  return ok;
}

int pair_work_moves(repo_t * repo, index_t * idx, stage_t * stage, paths_t * paths, int * move_path, int * skip_path) {
  for (int i = 0; i < idx->n; i++) move_path[i] = -1;
  for (int i = 0; i < idx->n; i++) {
    index_entry_t * e = &idx->v[i];
    char full[PATH_MAX];
    struct stat st;
    path_join(full, sizeof(full), repo->root, e->path);
    if (stage_find(stage, e->path) >= 0 || stage_find_move_from(stage, e->path) >= 0 || lstat(full, &st) == 0) continue;
    for (int j = 0; j < paths->n; j++) {
      if (skip_path[j] || index_find(idx, paths->v[j]) >= 0 || stage_contains_path(stage, paths->v[j])) continue;
      if (! work_path_matches_index(repo, e, paths->v[j])) continue;
      move_path[i] = j;
      skip_path[j] = 1;
      break;
    }
  }
  return 1;
}

int command_ls(repo_t * repo, int argc, char ** argv) {
  int tracked = 0, untracked = 0, ignored = 0;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--tracked") == 0) tracked = 1;
    else if (strcmp(argv[i], "--untracked") == 0) untracked = 1;
    else if (strcmp(argv[i], "--ignored") == 0) ignored = 1;
    else { fprintf(stderr, "usage: sc ls [--tracked] [--untracked] [--ignored]\n"); return 1; }
  }
  if (! tracked && ! untracked && ! ignored) tracked = untracked = 1;
  index_t idx = {0};
  stage_t stage = {0};
  if ((tracked || untracked) && ! load_index(repo, &idx)) return 1;
  if (untracked && ! load_stage(repo, &stage)) return 1;
  if (tracked) {
    int limit = (argc > 0) ? -1 : LIST_LIMIT;
    qsort(idx.v, idx.n, sizeof(index_entry_t), cmp_index_entries);
    for (int i = 0; i < idx.n && (limit < 0 || i < limit); i++)
      printf("tracked: %s\n", idx.v[i].path);
    if (limit >= 0 && idx.n > limit) printf("... + %d more tracked\n", idx.n - limit);
  }
  if (untracked) list_untracked(repo, &idx, &stage, (argc > 0) ? -1 : LIST_LIMIT);
  if (ignored) list_ignored(repo);
  stage_free(&stage);
  index_free(&idx);
  return 0;
}

int command_status(repo_t * repo) {
  index_t idx = {0};
  stage_t stage = {0};
  status_rows_t rows = {0};
  paths_t untracked = {0}, work_paths = {0};
  int * move_path = NULL, * skip_path = NULL, rc = 1;
  if (! load_index(repo, &idx)) return 1;
  if (! load_stage(repo, &stage)) return 1;
  move_path = malloc((idx.n ? idx.n : 1) * sizeof(int));
  progress_t scan;
  progress_start(&scan, "scan");
  scan_path(repo, "", &work_paths, &scan);
  progress_done(&scan);
  qsort(work_paths.v, work_paths.n, sizeof(char*), cmp_path_ptrs);
  skip_path = calloc(work_paths.n ? work_paths.n : 1, sizeof(int));
  if (! move_path || ! skip_path) goto done;
  pair_work_moves(repo, &idx, &stage, &work_paths, move_path, skip_path);
  progress_t progress;
  progress_start(&progress, "status");
  long long now = (long long) time(NULL);
  for (int i = 0; i < idx.n; i++) {
    index_entry_t * e = &idx.v[i];
    progress_tick(&progress, idx.n, e->path);
    char full[PATH_MAX], cache[PATH_MAX];
    bytes_t base = {0}, staged = {0}, work = {0};
    int base_type = e->type, staged_type = SOURCE_NONE, work_type = SOURCE_NONE;
    int base_exists = 0, staged_exists = 0, work_exists = 0;
    int si = stage_find(&stage, e->path);
    int move_si = stage_find_move_from(&stage, e->path);
    path_join(full, sizeof(full), repo->root, e->path);
    if (move_path[i] >= 0) {
      char moved[PATH_MAX * 2 + 5];
      snprintf(moved, sizeof(moved), "%s -> %s", e->path, work_paths.v[move_path[i]]);
      status_push(&rows, STATUS_UNSTAGED, "moved", moved, 0, 0, 0);
      continue;
    }
    if (si < 0 && move_si < 0) {
      struct stat st;
      if (lstat(full, &st) == 0) {
        int type = S_ISLNK(st.st_mode) ? SOURCE_LINK : (S_ISREG(st.st_mode) ? SOURCE_FILE : SOURCE_NONE);
        if (type == e->type && (unsigned long long) st.st_size == e->size && (long long) st.st_mtime == e->mtime && e->mtime < now) continue;
      }
    }
    cache_path(repo, e->path, cache);
    base_exists = read_bytes(cache, &base);
    if (! base_exists) empty_bytes(&base);
    if (move_si >= 0) {
      stage_entry_t * se = &stage.v[move_si];
      char moved[PATH_MAX * 2 + 5];
      read_staged(repo, se, &staged, &staged_type, &staged_exists);
      snprintf(moved, sizeof(moved), "%s -> %s", se->old_path, se->path);
      status_push(&rows, STATUS_STAGED, "moved", moved, 0, 0, 0);
      path_join(full, sizeof(full), repo->root, se->path);
      if (read_source(full, &work, &work_type)) work_exists = 1;
      else empty_bytes(&work);
      if (! bytes_equal(&staged, staged_type, staged_exists, &work, work_type, work_exists)) {
        const char * action = ! staged_exists && work_exists ? "added" :
          (staged_exists && ! work_exists ? "deleted" :
            (staged_type != work_type ? "type-change" :
              (staged_type == SOURCE_LINK ? "link-change" : "modified")));
        status_push_change(&rows, STATUS_UNSTAGED, action, se->path, &staged, staged_type, staged_exists, &work, work_type, work_exists);
      }
    } else if (si >= 0) {
      read_staged(repo, &stage.v[si], &staged, &staged_type, &staged_exists);
      const char * action = stage.v[si].action == STAGE_DEL ? "deleted" :
        (! base_exists ? "added" :
          (base_type != stage.v[si].type ? "type-change" :
            (base_type == SOURCE_LINK ? "link-change" : "modified")));
      status_push_change(&rows, STATUS_STAGED, action, e->path, &base, base_type, base_exists, &staged, staged_type, staged_exists);
    } else if (base_exists) {
      bytes_copy(&staged, &base);
      staged_type = base_type;
      staged_exists = 1;
    } else {
      empty_bytes(&staged);
    }
    if (move_si < 0) {
      if (read_source(full, &work, &work_type)) work_exists = 1;
      else empty_bytes(&work);
      if (! bytes_equal(&staged, staged_type, staged_exists, &work, work_type, work_exists)) {
        const char * action = ! staged_exists && work_exists ? "added" :
          (staged_exists && ! work_exists ? "deleted" :
            (staged_type != work_type ? "type-change" :
              (staged_type == SOURCE_LINK ? "link-change" : "modified")));
        status_push_change(&rows, STATUS_UNSTAGED, action, e->path, &staged, staged_type, staged_exists, &work, work_type, work_exists);
      }
    }
    free_bytes(&base);
    free_bytes(&staged);
    free_bytes(&work);
  }
  progress_done(&progress);
  for (int i = 0; i < stage.n; i++) {
    if (stage.v[i].action == STAGE_MOVE) continue;
    if (index_find(&idx, stage.v[i].path) < 0) {
      bytes_t empty = {0}, staged = {0};
      int staged_type = SOURCE_NONE, staged_exists = 0;
      empty_bytes(&empty);
      read_staged(repo, &stage.v[i], &staged, &staged_type, &staged_exists);
      status_push_change(&rows, STATUS_STAGED, "added", stage.v[i].path, &empty, SOURCE_NONE, 0, &staged, staged_type, staged_exists);
      free_bytes(&empty);
      free_bytes(&staged);
    }
  }
  int more = 0;
  int untracked_total = collect_untracked_from_paths(&idx, &stage, &work_paths, skip_path, STATUS_UNTRACKED_LIMIT, &untracked, &more);
  if (print_status_head(repo)) printf("\n");
  if (! rows.n && ! untracked_total) printf("nothing to commit\n");
  else print_status(&rows, &untracked, more);
  printf("\n");
  rc = 0;
done:
  free(move_path);
  free(skip_path);
  paths_free(&work_paths);
  paths_free(&untracked);
  status_rows_free(&rows);
  stage_free(&stage);
  index_free(&idx);
  return rc;
}

int write_commit_file(FILE * fp, const char * path, int kind, int old_type, int new_type, bytes_t * old, bytes_t * new, edits_t * edits) {
  fprintf(fp, "file %zu\n", strlen(path));
  fwrite(path, 1, strlen(path), fp);
  fprintf(fp, "\nkind %d\nold_type %d\nnew_type %d\nold_size %zu\nnew_size %zu\nold_hash %016llx\nnew_hash %016llx\nedits %d\n",
    kind, old_type, new_type, old->n, new->n,
    (unsigned long long) hash_bytes(old->data, old->n),
    (unsigned long long) hash_bytes(new->data, new->n),
    edits->n);
  for (int i = 0; i < edits->n; i++) {
    edit_t * e = &edits->v[i];
    fprintf(fp, "edit %zu %zu %zu\n", e->off, e->new_n, e->old_n);
    fwrite(e->old, 1, e->old_n, fp);
    fprintf(fp, "\n");
  }
  return 1;
}

int write_commit_move(FILE * fp, const char * old_path, const char * new_path, int type, unsigned long long size, uint64_t hash) {
  fprintf(fp, "file %zu\n", strlen(new_path));
  fwrite(new_path, 1, strlen(new_path), fp);
  fprintf(fp, "\nkind 3\nold_path %zu\n", strlen(old_path));
  fwrite(old_path, 1, strlen(old_path), fp);
  fprintf(fp, "\ntype %d\nsize %llu\nhash %016llx\n", type, size, (unsigned long long) hash);
  return 1;
}

void print_action_msg(const char * action, unsigned long long id, long long t, char * msg) {
  time_t tt = (time_t) t;
  char * ts = ctime(&tt);
  if (ts) ts[strcspn(ts, "\n")] = '\0';
  if (! msg || ! strchr(msg, '\n')) {
    printf("%s %llu %s %s\n", action, id, ts ? ts : "unknown time", msg ? msg : "");
    return;
  }
  printf("%s %llu %s\n", action, id, ts ? ts : "unknown time");
  for (char * p = msg; *p;) {
    char * e = strchr(p, '\n');
    size_t n = e ? (size_t) (e - p) : strlen(p);
    if (n) { printf("  "); fwrite(p, 1, n, stdout); printf("\n"); }
    if (! e) break;
    p = e + 1;
  }
}

int commit_stage(repo_t * repo, index_t * idx, stage_t * stage, long long t, char * msg, const char * action, int allow_empty, int quiet) {
  index_t next = {0};
  for (int i = 0; i < idx->n; i++) {
    index_entry_t e = idx->v[i];
    e.path = xstrdup(idx->v[i].path);
    if (! e.path || ! index_push(&next, e)) return 1;
  }
  unsigned long long parent = read_head(repo), id = next_commit_id(repo);
  char cpath[PATH_MAX];
  snprintf(cpath, sizeof(cpath), "%s/commits/%016llx.sc", repo->sc, id);
  FILE * fp = fopen(cpath, "wb");
  if (! fp) return 1;
  fprintf(fp, "SC1\ncommit %llu\nparent %llu\ntime %lld\nmessage %zu\n%s\nfiles 0\n",
    id, parent, t, strlen(msg), msg);
  int * move_pair = malloc((stage->n ? stage->n : 1) * sizeof(int));
  int * skip_stage = calloc(stage->n ? stage->n : 1, sizeof(int));
  if (! move_pair || ! skip_stage) return 1;
  pair_staged_moves(idx, stage, move_pair, skip_stage);
  int changed = 0;
  for (int i = 0; i < stage->n; i++) {
    stage_entry_t * se = &stage->v[i];
    if (skip_stage[i]) continue;
    if (se->action == STAGE_MOVE || move_pair[i] >= 0) {
      stage_entry_t * add = move_pair[i] >= 0 ? &stage->v[move_pair[i]] : se;
      const char * old_path = se->action == STAGE_MOVE ? se->old_path : se->path;
      const char * new_path = add->path;
      int old_j = index_find(&next, old_path);
      int new_j = index_find(&next, new_path);
      char old_cache[PATH_MAX], new_cache[PATH_MAX];
      if (! old_path || old_j < 0) { fclose(fp); return 1; }
      write_commit_move(fp, old_path, new_path, add->type, add->size, add->content_hash);
      cache_path(repo, old_path, old_cache);
      cache_path(repo, new_path, new_cache);
      rename(old_cache, new_cache);
      index_entry_t ne = {hash_string(new_path), add->content_hash, add->size, add->mtime, add->type, 0, xstrdup(new_path)};
      if (! ne.path) { fclose(fp); return 1; }
      free(next.v[old_j].path);
      memmove(next.v + old_j, next.v + old_j + 1, (next.n - old_j - 1) * sizeof(index_entry_t));
      next.n--;
      if (new_j > old_j) new_j--;
      if (new_j >= 0) {
        free(next.v[new_j].path);
        next.v[new_j] = ne;
      } else if (! index_push(&next, ne)) {
        free(ne.path);
        fclose(fp);
        return 1;
      }
      changed++;
      continue;
    }
    int j = index_find(&next, se->path);
    char cache[PATH_MAX], spath[PATH_MAX];
    bytes_t old = {0}, new = {0};
    edits_t edits = {0};
    int old_exists, new_exists, old_type = (j >= 0) ? next.v[j].type : SOURCE_NONE, new_type = se->type, kind = 0;
    cache_path(repo, se->path, cache);
    staged_path(repo, se->path, spath);
    old_exists = read_bytes(cache, &old);
    new_exists = se->action != STAGE_DEL && read_bytes(spath, &new);
    if (! old_exists) { old.data = malloc(1); old.n = 0; old_type = SOURCE_NONE; kind = 1; }
    if (! new_exists) { new.data = malloc(1); new.n = 0; new_type = SOURCE_NONE; kind = old_exists ? 2 : kind; }
    if (! (old_exists && new_exists && old_type == new_type && old.n == new.n && memcmp(old.data, new.data, old.n) == 0)) {
      if (! build_reverse_edits(kind, &old, &new, &edits)) {
        edits_free(&edits);
        free_bytes(&old);
        free_bytes(&new);
        fclose(fp);
        return 1;
      }
      write_commit_file(fp, se->path, kind, old_type, new_type, &old, &new, &edits);
      changed++;
      if (new_exists) {
        write_bytes(cache, new.data, new.n);
        index_entry_t ne = {se->path_hash, se->content_hash, se->size, se->mtime, new_type, 0, xstrdup(se->path)};
        if (j >= 0) {
          free(next.v[j].path);
          next.v[j] = ne;
        } else index_push(&next, ne);
      } else if (j >= 0) {
        unlink(cache);
        free(next.v[j].path);
        memmove(next.v + j, next.v + j + 1, (next.n - j - 1) * sizeof(index_entry_t));
        next.n--;
      }
    }
    edits_free(&edits);
    free_bytes(&old);
    free_bytes(&new);
  }
  free(move_pair);
  free(skip_stage);
  fclose(fp);
  if (! changed && ! allow_empty) {
    unlink(cpath);
    printf("nothing to commit\n");
    index_free(&next);
    return 1;
  }
  FILE * in = fopen(cpath, "rb");
  char final[PATH_MAX];
  snprintf(final, sizeof(final), "%s/commits/%016llx.tmp", repo->sc, id);
  FILE * out = fopen(final, "wb");
  char line[8192];
  size_t msg_n = 0;
  for (int i = 0; i < 5 && fgets(line, sizeof(line), in); i++) {
    if (i == 4) sscanf(line, "message %zu", &msg_n);
    fputs(line, out);
  }
  char * mbuf = malloc(msg_n ? msg_n : 1);
  if (! mbuf) return 1;
  fread(mbuf, 1, msg_n, in);
  fwrite(mbuf, 1, msg_n, out);
  free(mbuf);
  fgetc(in);
  fputc('\n', out);
  fprintf(out, "files %d\n", changed);
  fgets(line, sizeof(line), in);
  size_t n;
  while ((n = fread(line, 1, sizeof(line), in))) fwrite(line, 1, n, out);
  fclose(in);
  fclose(out);
  rename(final, cpath);
  save_index(repo, &next);
  clear_stage(repo, stage);
  write_head(repo, id);
  if (! quiet) print_action_msg(action, id, t, msg);
  index_free(&next);
  return 0;
}

int command_commit(repo_t * repo, int argc, char ** argv) {
  char * msg = NULL;
  for (int i = 0; i + 1 < argc; i++) if (strcmp(argv[i], "-m") == 0) msg = argv[i + 1];
  if (! msg) { fprintf(stderr, "usage: sc commit -m \"message\"\n"); return 1; }
  index_t idx = {0};
  stage_t stage = {0};
  if (! load_index(repo, &idx)) return 1;
  if (! load_stage(repo, &stage)) return 1;
  if (! stage.n && ! (stage_unstaged(repo, &idx, &stage) && save_index(repo, &idx) && save_stage(repo, &stage))) {
    index_free(&idx); stage_free(&stage); return 1;
  }
  int rc = commit_stage(repo, &idx, &stage, (long long) time(NULL), msg, "commit", 0, 0);
  index_free(&idx);
  stage_free(&stage);
  return rc;
}

int read_line(FILE * fp, char * line, size_t n) {
  return fgets(line, n, fp) != NULL;
}

int read_edit(FILE * fp, edit_t * e) {
  char line[256];
  if (! read_line(fp, line, sizeof(line))) return 0;
  if (sscanf(line, "edit %zu %zu %zu", &e->off, &e->new_n, &e->old_n) != 3) return 0;
  e->old = malloc(e->old_n ? e->old_n : 1);
  if (! e->old) return 0;
  if (fread(e->old, 1, e->old_n, fp) != e->old_n) return 0;
  fgetc(fp);
  return 1;
}

void commit_free(commit_t * c) {
  free(c->msg);
  for (int i = 0; i < c->n; i++) {
    free(c->files[i].old_path);
    free(c->files[i].path);
    edits_free(&c->files[i].edits);
  }
  free(c->files);
  memset(c, 0, sizeof(*c));
}

int read_commit(repo_t * repo, unsigned long long id, commit_t * c) {
  char path[PATH_MAX], line[8192];
  snprintf(path, sizeof(path), "%s/commits/%016llx.sc", repo->sc, id);
  FILE * fp = fopen(path, "rb");
  if (! fp) return 0;
  memset(c, 0, sizeof(*c));
  c->id = id;
  int files = 0;
  read_line(fp, line, sizeof(line));
  read_line(fp, line, sizeof(line));
  read_line(fp, line, sizeof(line)); sscanf(line, "parent %llu", &c->parent);
  read_line(fp, line, sizeof(line)); sscanf(line, "time %lld", &c->time);
  read_line(fp, line, sizeof(line)); size_t msg_n = 0; sscanf(line, "message %zu", &msg_n);
  c->msg = malloc(msg_n + 1);
  if (! c->msg) { fclose(fp); return 0; }
  fread(c->msg, 1, msg_n, fp);
  c->msg[msg_n] = '\0';
  fgetc(fp);
  read_line(fp, line, sizeof(line)); sscanf(line, "files %d", &files);
  for (int i = 0; i < files; i++) {
    commit_file_t f;
    long start = ftell(fp);
    size_t path_n = 0;
    int nedits = 0;
    memset(&f, 0, sizeof(f));
    read_line(fp, line, sizeof(line)); sscanf(line, "file %zu", &path_n);
    f.path = malloc(path_n + 1);
    if (! f.path) { fclose(fp); commit_free(c); return 0; }
    fread(f.path, 1, path_n, fp); f.path[path_n] = '\0'; fgetc(fp);
    read_line(fp, line, sizeof(line)); sscanf(line, "kind %d", &f.kind);
    if (f.kind == 3) {
      read_line(fp, line, sizeof(line)); sscanf(line, "old_path %zu", &path_n);
      f.old_path = malloc(path_n + 1);
      if (! f.old_path) { free(f.path); fclose(fp); commit_free(c); return 0; }
      fread(f.old_path, 1, path_n, fp); f.old_path[path_n] = '\0'; fgetc(fp);
      read_line(fp, line, sizeof(line)); sscanf(line, "type %d", &f.new_type);
      f.old_type = f.new_type;
      read_line(fp, line, sizeof(line)); sscanf(line, "size %llu", &f.size);
      f.old_size = f.size;
      f.new_size = f.size;
      read_line(fp, line, sizeof(line)); sscanf(line, "hash %llx", (unsigned long long*) &f.content_hash);
      long end = ftell(fp);
      if (start >= 0 && end >= start) f.stored_size = (unsigned long long) (end - start);
      commit_file_t * v = realloc(c->files, (c->n + 1) * sizeof(commit_file_t));
      if (! v) {
        free(f.old_path);
        free(f.path);
        fclose(fp);
        commit_free(c);
        return 0;
      }
      c->files = v;
      c->files[c->n++] = f;
      continue;
    }
    read_line(fp, line, sizeof(line)); sscanf(line, "old_type %d", &f.old_type);
    read_line(fp, line, sizeof(line)); sscanf(line, "new_type %d", &f.new_type);
    read_line(fp, line, sizeof(line)); sscanf(line, "old_size %llu", &f.old_size);
    read_line(fp, line, sizeof(line)); sscanf(line, "new_size %llu", &f.new_size);
    read_line(fp, line, sizeof(line));
    read_line(fp, line, sizeof(line));
    read_line(fp, line, sizeof(line)); sscanf(line, "edits %d", &nedits);
    for (int j = 0; j < nedits; j++) {
      edit_t e = {0};
      if (! read_edit(fp, &e) || ! edits_push(&f.edits, e.off, e.new_n, e.old, e.old_n)) {
        free(e.old);
        free(f.path);
        edits_free(&f.edits);
        fclose(fp);
        commit_free(c);
        return 0;
      }
      free(e.old);
    }
    long end = ftell(fp);
    if (start >= 0 && end >= start) f.stored_size = (unsigned long long) (end - start);
    commit_file_t * v = realloc(c->files, (c->n + 1) * sizeof(commit_file_t));
    if (! v) {
      free(f.old_path);
      free(f.path);
      edits_free(&f.edits);
      fclose(fp);
      commit_free(c);
      return 0;
    }
    c->files = v;
    c->files[c->n++] = f;
  }
  fclose(fp);
  return 1;
}

commit_file_t * commit_file(commit_t * c, const char * path) {
  for (int i = 0; i < c->n; i++)
    if (strcmp(c->files[i].path, path) == 0 ||
        (c->files[i].old_path && strcmp(c->files[i].old_path, path) == 0))
      return &c->files[i];
  return NULL;
}

int reverse_state(commit_file_t * f, bytes_t * b, int * type, int * exists) {
  if (f->kind == 1) {
    free_bytes(b);
    empty_bytes(b);
    *type = SOURCE_NONE;
    *exists = 0;
    return b->data != NULL;
  }
  bytes_t out = {0};
  if (! apply_edits(b, &f->edits, &out)) return 0;
  free_bytes(b);
  *b = out;
  *type = f->old_type;
  *exists = 1;
  return 1;
}

int state_at_commit(repo_t * repo, index_t * idx, const char * path, unsigned long long target, bytes_t * b, int * type, int * exists) {
  char cache[PATH_MAX];
  int j = index_find(idx, path);
  *exists = 0;
  *type = SOURCE_NONE;
  empty_bytes(b);
  if (j >= 0 && b->data) {
    cache_path(repo, path, cache);
    free_bytes(b);
    if (! read_bytes(cache, b)) empty_bytes(b);
    else { *type = idx->v[j].type; *exists = 1; }
  }
  if (! b->data) return 0;
  int found = target == read_head(repo);
  for (unsigned long long h = read_head(repo); h && h != target;) {
    commit_t c = {0};
    unsigned long long id = h;
    if (! read_commit(repo, h, &c)) return 0;
    commit_file_t * f = commit_file(&c, path);
    h = c.parent;
    if (h == target) found = 1;
    int ok = 1;
    if (f && f->kind == 3 && f->old_path && strcmp(path, f->old_path) == 0) {
      free_bytes(b);
      ok = state_at_commit(repo, idx, f->path, id, b, type, exists);
    } else if (f && f->kind == 3) {
      free_bytes(b);
      empty_bytes(b);
      *type = SOURCE_NONE;
      *exists = 0;
      ok = b->data != NULL;
    } else if (f) {
      ok = reverse_state(f, b, type, exists);
    }
    commit_free(&c);
    if (! ok) return 0;
  }
  return target == 0 || found;
}

int paths_push_unique(paths_t * ps, const char * path) {
  for (int i = 0; i < ps->n; i++)
    if (strcmp(ps->v[i], path) == 0) return 1;
  return paths_push(ps, path);
}

int paths_push_commit_file(paths_t * ps, commit_file_t * f) {
  return paths_push_unique(ps, f->path) &&
    (! f->old_path || paths_push_unique(ps, f->old_path));
}

int binary_like(bytes_t * b) {
  for (size_t i = 0; i < b->n; i++)
    if ((b->data[i] == 0) || (b->data[i] < 32 && b->data[i] != '\n' && b->data[i] != '\r' && b->data[i] != '\t'))
      return 1;
  return 0;
}

int split_lines(bytes_t * b, line_t ** out) {
  int n = 0, size = 0;
  *out = NULL;
  for (size_t i = 0; i < b->n;) {
    if (n == size) {
      size = size ? 2 * size : INITIAL_SIZE;
      line_t * v = realloc(*out, size * sizeof(line_t));
      if (! v) return -1;
      *out = v;
    }
    size_t j = i;
    while (j < b->n && b->data[j++] != '\n') {}
    (*out)[n++] = (line_t){b->data + i, j - i};
    i = j;
  }
  return n;
}

int line_eq(line_t * a, line_t * b) {
  return a->n == b->n && memcmp(a->s, b->s, a->n) == 0;
}

size_t line_chars(line_t * l) {
  return (l->n && l->s[l->n - 1] == '\n') ? l->n - 1 : l->n;
}

void print_diff_line(const char * color, char prefix, line_t * l, const char * reset) {
  printf("%s%c ", color, prefix);
  fwrite(l->s, 1, l->n, stdout);
  if (l->n == 0 || l->s[l->n - 1] != '\n') printf("\n");
  printf("%s", reset);
}

int same_indent(line_t * a, line_t * b) {
  size_t i = 0;
  while (i < a->n && i < b->n && (a->s[i] == ' ' || a->s[i] == '\t') && a->s[i] == b->s[i]) i++;
  return (i == 0) || (i < a->n && i < b->n);
}

double line_similarity(line_t * a, line_t * b) {
  size_t an = line_chars(a), bn = line_chars(b);
  if (! an && ! bn) return 1.0;
  if (! an || ! bn || an + bn > 4096) return 0.0;
  int cols = (int) bn + 1;
  unsigned short * dp = calloc((an + 1) * (bn + 1), sizeof(unsigned short));
  if (! dp) return 0.0;
  for (int i = (int) an - 1; i >= 0; i--)
    for (int j = (int) bn - 1; j >= 0; j--)
      dp[i * cols + j] = (a->s[i] == b->s[j]) ? 1 + dp[(i + 1) * cols + j + 1] :
        (dp[(i + 1) * cols + j] > dp[i * cols + j + 1] ? dp[(i + 1) * cols + j] : dp[i * cols + j + 1]);
  double score = (2.0 * dp[0]) / (double) (an + bn);
  free(dp);
  return score;
}

void print_span(const char * color, const unsigned char * s, size_t n, const char * reset) {
  if (n) {
    printf("%s", color);
    fwrite(s, 1, n, stdout);
    printf("%s", reset);
  }
}

size_t utf8_char_len(const unsigned char * s, size_t n) {
  if (! n || s[0] < 0x80) return n ? 1 : 0;
  if ((s[0] & 0xe0) == 0xc0 && n >= 2 && (s[1] & 0xc0) == 0x80) return 2;
  if ((s[0] & 0xf0) == 0xe0 && n >= 3 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) return 3;
  if ((s[0] & 0xf8) == 0xf0 && n >= 4 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) return 4;
  return 1;
}

int has_change_char(unsigned char c) {
  return c != '\r';
}

int has_visible_char(const unsigned char * s, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return 1;
  return 0;
}

void print_changed_span(const char * color, const unsigned char * s, size_t n, const char * reset, char marker) {
  if (! n) return;
  (void) marker;
  int visible = has_visible_char(s, n);
  for (size_t i = 0; i < n;) {
    if (s[i] == ' ' || s[i] == '\t') {
      if (visible) {
        fwrite(s + i, 1, 1, stdout);
      } else if (color[0]) {
        printf("%s\033[7m", color);
        fwrite(s + i, 1, 1, stdout);
        printf("%s", reset);
      } else {
        fwrite(s + i, 1, 1, stdout);
      }
      i++;
    } else {
      size_t m = utf8_char_len(s + i, n - i);
      printf("%s", color);
      fwrite(s + i, 1, m, stdout);
      printf("%s", reset);
      i += m;
    }
  }
}

int old_change(unsigned short * dp, int cols, int an, int bn, int i, int j) {
  return j >= bn || (i < an && dp[(i + 1) * cols + j] >= dp[i * cols + j + 1]);
}

int new_change(unsigned short * dp, int cols, int an, int bn, int i, int j) {
  return i >= an || (j < bn && dp[i * cols + j + 1] > dp[(i + 1) * cols + j]);
}

int intraline_visible(line_t * old, line_t * new, unsigned short * dp, int cols, int added) {
  int an = (int) line_chars(old), bn = (int) line_chars(new);
  if (added) {
    for (int i = 0, j = 0; j < bn;) {
      if (i < an && old->s[i] == new->s[j]) { i++; j++; }
      else if (new_change(dp, cols, an, bn, i, j)) { if (has_change_char(new->s[j])) return 1; j++; }
      else i++;
    }
  } else {
    for (int i = 0, j = 0; i < an;) {
      if (j < bn && old->s[i] == new->s[j]) { i++; j++; }
      else if (old_change(dp, cols, an, bn, i, j)) { if (has_change_char(old->s[i])) return 1; i++; }
      else j++;
    }
  }
  return 0;
}

int intraline_pair_lines(line_t * old, line_t * new) {
  size_t an = line_chars(old), bn = line_chars(new);
  if (an + bn > 4096) return 2;
  int cols = (int) bn + 1;
  unsigned short * dp = calloc((an + 1) * (bn + 1), sizeof(unsigned short));
  if (! dp) return 2;
  for (int i = (int) an - 1; i >= 0; i--)
    for (int j = (int) bn - 1; j >= 0; j--)
      dp[i * cols + j] = (old->s[i] == new->s[j]) ? 1 + dp[(i + 1) * cols + j + 1] :
        (dp[(i + 1) * cols + j] > dp[i * cols + j + 1] ? dp[(i + 1) * cols + j] : dp[i * cols + j + 1]);
  int lines = intraline_visible(old, new, dp, cols, 0) + intraline_visible(old, new, dp, cols, 1);
  free(dp);
  return lines;
}

int print_intraline_pair(line_t * old, line_t * new, const char * red, const char * green, const char * dim, const char * reset) {
  size_t an = line_chars(old), bn = line_chars(new);
  if (an + bn > 4096) {
    print_diff_line(red, '-', old, reset);
    print_diff_line(green, '+', new, reset);
    return 2;
  }
  int cols = (int) bn + 1;
  unsigned short * dp = calloc((an + 1) * (bn + 1), sizeof(unsigned short));
  if (! dp) {
    print_diff_line(red, '-', old, reset);
    print_diff_line(green, '+', new, reset);
    return 2;
  }
  for (int i = (int) an - 1; i >= 0; i--)
    for (int j = (int) bn - 1; j >= 0; j--)
      dp[i * cols + j] = (old->s[i] == new->s[j]) ? 1 + dp[(i + 1) * cols + j + 1] :
        (dp[(i + 1) * cols + j] > dp[i * cols + j + 1] ? dp[(i + 1) * cols + j] : dp[i * cols + j + 1]);
  int show_old = intraline_visible(old, new, dp, cols, 0), show_new = intraline_visible(old, new, dp, cols, 1);
  if (show_old) {
    printf("%s- %s", red, reset);
    for (int i = 0, j = 0; i < (int) an;) {
      if (j < (int) bn && old->s[i] == new->s[j]) {
        int start = i;
        while (i < (int) an && j < (int) bn && old->s[i] == new->s[j]) { i++; j++; }
        print_span(dim, old->s + start, i - start, reset);
      }
      else if (old_change(dp, cols, (int) an, (int) bn, i, j)) {
        int start = i;
        while (i < (int) an && !(j < (int) bn && old->s[i] == new->s[j]) && old_change(dp, cols, (int) an, (int) bn, i, j)) i++;
        print_changed_span(red, old->s + start, i - start, reset, '-');
      }
      else j++;
    }
    printf("\n");
  }
  if (show_new) {
    printf("%s+ %s", green, reset);
    for (int i = 0, j = 0; j < (int) bn;) {
      if (i < (int) an && old->s[i] == new->s[j]) {
        int start = j;
        while (i < (int) an && j < (int) bn && old->s[i] == new->s[j]) { i++; j++; }
        print_span(dim, new->s + start, j - start, reset);
      }
      else if (new_change(dp, cols, (int) an, (int) bn, i, j)) {
        int start = j;
        while (j < (int) bn && !(i < (int) an && old->s[i] == new->s[j]) && new_change(dp, cols, (int) an, (int) bn, i, j)) j++;
        print_changed_span(green, new->s + start, j - start, reset, '+');
      }
      else i++;
    }
    printf("\n");
  }
  free(dp);
  return show_old + show_new;
}

void print_simple_lines(bytes_t * b, char prefix, const char * color, const char * reset, int full) {
  line_t * lines = NULL;
  int n = split_lines(b, &lines);
  if (! full && n > DIFF_LINE_LIMIT) printf("  text diff omitted (would show %d diff lines; use --full)\n", n);
  else for (int i = 0; i < n; i++) print_diff_line(color, prefix, &lines[i], reset);
  free(lines);
}

#define DIFF_ANCHOR_SIZE 16

typedef struct {
  size_t old_off;
  size_t new_off;
  size_t n;
} anchor_t;

typedef struct {
  anchor_t * v;
  int n;
  int size;
} anchors_t;

int anchor_push(anchors_t * as, size_t old_off, size_t new_off, size_t n) {
  if (as->n == as->size) {
    as->size = as->size ? 2 * as->size : INITIAL_SIZE;
    anchor_t * v = realloc(as->v, as->size * sizeof(anchor_t));
    if (! v) return 0;
    as->v = v;
  }
  as->v[as->n++] = (anchor_t){old_off, new_off, n};
  return 1;
}

int prose_byte(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 128;
}

int append_byte(bytes_t * b, size_t * cap, unsigned char c) {
  if (b->n + 1 > *cap) {
    *cap = *cap ? 2 * *cap : 256;
    unsigned char * data = realloc(b->data, *cap);
    if (! data) return 0;
    b->data = data;
  }
  b->data[b->n++] = c;
  return 1;
}

int normalize_hunk(line_t * lines, int start, int n, bytes_t * out, int * words, int * code_like, int * width) {
  size_t cap = 0;
  int in_word = 0, nonblank = 0, total_width = 0;
  *out = (bytes_t){0};
  *words = *code_like = *width = 0;
  for (int i = 0; i < n; i++) {
    line_t * l = &lines[start + i];
    size_t m = line_chars(l), p = 0;
    while (p < m && (l->s[p] == ' ' || l->s[p] == '\t')) p++;
    if (p < m) {
      nonblank++;
      total_width += (int) m;
      if (p || memchr(l->s, '\t', m) || memchr(l->s, ';', m) || memchr(l->s, '{', m) || memchr(l->s, '}', m)) *code_like = 1;
    }
    for (p = 0; p < m; p++) {
      unsigned char c = l->s[p];
      int space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
      if (space) {
        in_word = 0;
        if (out->n && out->data[out->n - 1] != ' ' && ! append_byte(out, &cap, ' ')) goto fail;
      } else {
        if (prose_byte(c) && ! in_word) { (*words)++; in_word = 1; }
        else if (! prose_byte(c)) in_word = 0;
        if (! append_byte(out, &cap, c)) goto fail;
      }
    }
    if (out->n && out->data[out->n - 1] != ' ' && ! append_byte(out, &cap, ' ')) goto fail;
    in_word = 0;
  }
  while (out->n && out->data[out->n - 1] == ' ') out->n--;
  *width = nonblank ? total_width / nonblank : 72;
  if (*width < 60) *width = 60;
  if (*width > 100) *width = 100;
  return 1;
fail:
  free(out->data);
  out->data = NULL;
  out->n = 0;
  return 0;
}

int build_anchors(bytes_t * old, bytes_t * new, anchors_t * anchors, progress_t * progress) {
  if (old->n < DIFF_ANCHOR_SIZE || new->n < DIFF_ANCHOR_SIZE) return 0;
  int blocks = (int) (old->n - DIFF_ANCHOR_SIZE + 1), nbuckets = 1;
  while (nbuckets < blocks * 2) nbuckets *= 2;
  int * buckets = malloc(nbuckets * sizeof(int));
  block_match_t * matches = malloc(blocks * sizeof(block_match_t));
  if (! buckets || ! matches) { free(buckets); free(matches); return 0; }
  for (int i = 0; i < nbuckets; i++) buckets[i] = -1;
  for (int i = 0; i < blocks; i++) {
    matches[i].weak = weak_sum(old->data + i, DIFF_ANCHOR_SIZE);
    matches[i].strong = hash_bytes(old->data + i, DIFF_ANCHOR_SIZE);
    matches[i].off = (size_t) i;
    int b = (int) (matches[i].weak & (uint32_t) (nbuckets - 1));
    matches[i].next = buckets[b];
    buckets[b] = i;
  }
  size_t old_pos = 0, new_pos = 0, i = 0, matched = 0;
  uint32_t w = weak_sum(new->data, DIFF_ANCHOR_SIZE);
  while (i + DIFF_ANCHOR_SIZE <= new->n) {
    progress_tick(progress, -1, NULL);
    int b = (int) (w & (uint32_t) (nbuckets - 1)), checked = 0, have_strong = 0;
    uint64_t strong = 0;
    size_t best = (size_t) -1;
    for (int j = buckets[b]; j >= 0; j = matches[j].next) {
      if (++checked > MAX_BLOCK_CANDIDATES) break;
      if (matches[j].weak != w || matches[j].off < old_pos) continue;
      if (! have_strong) { strong = hash_bytes(new->data + i, DIFF_ANCHOR_SIZE); have_strong = 1; }
      if (matches[j].strong == strong && memcmp(old->data + matches[j].off, new->data + i, DIFF_ANCHOR_SIZE) == 0 && matches[j].off < best) best = matches[j].off;
    }
    if (best != (size_t) -1) {
      size_t oo = best, nn = i, len = DIFF_ANCHOR_SIZE;
      while (oo > old_pos && nn > new_pos && old->data[oo - 1] == new->data[nn - 1]) { oo--; nn--; len++; }
      while (oo + len < old->n && nn + len < new->n && old->data[oo + len] == new->data[nn + len]) len++;
      if (! anchor_push(anchors, oo, nn, len)) break;
      matched += len;
      old_pos = oo + len;
      new_pos = nn + len;
      i = new_pos;
      if (i + DIFF_ANCHOR_SIZE <= new->n) w = weak_sum(new->data + i, DIFF_ANCHOR_SIZE);
      continue;
    }
    if (i + DIFF_ANCHOR_SIZE >= new->n) break;
    uint32_t a = (w & 0xffff) - new->data[i] + new->data[i + DIFF_ANCHOR_SIZE];
    uint32_t bb = (w >> 16) - (uint32_t) DIFF_ANCHOR_SIZE * new->data[i] + a;
    w = (a & 0xffff) | (bb << 16);
    i++;
  }
  free(buckets);
  free(matches);
  return matched >= DIFF_ANCHOR_SIZE;
}

int print_marked_text(const unsigned char * s, size_t n, unsigned char * same, char prefix, const char * change_color, const char * same_color, const char * reset, int width, int strike, int emit) {
  int shown = 0;
  size_t p = 0;
  while (p < n) {
    while (p < n && s[p] == ' ') p++;
    if (p >= n) break;
    size_t end = p + (size_t) width;
    if (end >= n) end = n;
    else {
      size_t cut = end;
      while (cut > p && s[cut] != ' ') cut--;
      if (cut > p) end = cut;
    }
    while (end > p && s[end - 1] == ' ') end--;
    int changed = 0;
    for (size_t i = p; i < end && ! changed; i++) changed = ! same[i];
    if (! changed) { p = end; continue; }
    if (emit) {
      printf("%s%c %s", change_color, prefix, reset);
      for (size_t i = p; i < end;) {
        size_t m = utf8_char_len(s + i, end - i);
        const char * color = same[i] ? same_color : change_color;
        printf("%s%s", color, (! same[i] && strike && color[0]) ? "\033[9m" : "");
        fwrite(s + i, 1, m, stdout);
        printf("%s", reset);
        i += m;
      }
      printf("%s\n", reset);
    }
    shown++;
    p = end;
  }
  return shown;
}

int print_anchor_hunk(line_t * a, int ai, int an, line_t * b, int bi, int bn, const char * red, const char * green, const char * dim, const char * reset, int emit, progress_t * progress) {
  bytes_t old = {0}, new = {0};
  anchors_t anchors = {0};
  unsigned char * same_old = NULL, * same_new = NULL;
  int old_words = 0, new_words = 0, old_code = 0, new_code = 0, old_width = 72, new_width = 72, shown = 0;
  if (! normalize_hunk(a, ai, an, &old, &old_words, &old_code, &old_width) ||
      ! normalize_hunk(b, bi, bn, &new, &new_words, &new_code, &new_width)) goto done;
  if (old_code || new_code || old_words < 12 || new_words < 12 || ! build_anchors(&old, &new, &anchors, progress)) goto done;
  int width = old_width > new_width ? old_width : new_width;
  same_old = calloc(old.n ? old.n : 1, 1);
  same_new = calloc(new.n ? new.n : 1, 1);
  if (! same_old || ! same_new) goto done;
  for (int i = 0; i < anchors.n; i++) {
    anchor_t * m = &anchors.v[i];
    memset(same_old + m->old_off, 1, m->n);
    memset(same_new + m->new_off, 1, m->n);
  }
  shown += print_marked_text(old.data, old.n, same_old, '-', red, dim, reset, width, 1, emit);
  shown += print_marked_text(new.data, new.n, same_new, '+', green, dim, reset, width, 0, emit);
done:
  free(same_old);
  free(same_new);
  free(old.data);
  free(new.data);
  free(anchors.v);
  return shown;
}

int print_changed_hunk(line_t * a, int ai, int an, line_t * b, int bi, int bn, const char * red, const char * green, const char * dim, const char * reset, int emit, progress_t * progress) {
  int shown = print_anchor_hunk(a, ai, an, b, bi, bn, red, green, dim, reset, emit, progress);
  if (shown) return shown;
  for (int i = 0; i < an; i++) { if (emit) print_diff_line(red, '-', &a[ai + i], reset); shown++; }
  for (int j = 0; j < bn; j++) { if (emit) print_diff_line(green, '+', &b[bi + j], reset); shown++; }
  return shown;
}

void print_ellipsis(const char * dim, const char * reset) {
  printf("%s...%s\n", dim, reset);
}

int print_context_lines(line_t * lines, int start, int n, const char * dim, const char * reset, int emit) {
  for (int i = 0; emit && i < n; i++) print_diff_line(dim, ' ', &lines[start + i], reset);
  return n;
}

int print_context_run(line_t * lines, int start, int n, int leading, int trailing, const char * dim, const char * reset, int emit) {
  int shown = 0;
  if (n <= 0) return 0;
  if (n <= 2 * DIFF_CONTEXT_LINES && ! leading && ! trailing)
    return print_context_lines(lines, start, n, dim, reset, emit);
  if (leading) {
    if (n > DIFF_CONTEXT_LINES) { if (emit) print_ellipsis(dim, reset); shown++; }
    shown += print_context_lines(lines, start + (n > DIFF_CONTEXT_LINES ? n - DIFF_CONTEXT_LINES : 0), n > DIFF_CONTEXT_LINES ? DIFF_CONTEXT_LINES : n, dim, reset, emit);
  } else if (trailing) {
    shown += print_context_lines(lines, start, n > DIFF_CONTEXT_LINES ? DIFF_CONTEXT_LINES : n, dim, reset, emit);
    if (n > DIFF_CONTEXT_LINES) { if (emit) print_ellipsis(dim, reset); shown++; }
  } else {
    shown += print_context_lines(lines, start, DIFF_CONTEXT_LINES, dim, reset, emit);
    if (emit) print_ellipsis(dim, reset);
    shown++;
    shown += print_context_lines(lines, start + n - DIFF_CONTEXT_LINES, DIFF_CONTEXT_LINES, dim, reset, emit);
  }
  return shown;
}

int render_lcs_diff(line_t * a, int ao, int an, line_t * b, int bo, int bn, int pre, int suf, int * dp, int cols, const char * red, const char * green, const char * dim, const char * reset, int emit, progress_t * progress) {
  int shown = print_context_run(a, 0, pre, 1, 0, dim, reset, emit);
  int changed = 0;
  for (int i = 0, j = 0; i < an || j < bn;) {
    if (i < an && j < bn && line_eq(&a[ao + i], &b[bo + j])) {
      int dj = j;
      while (i < an && j < bn && line_eq(&a[ao + i], &b[bo + j])) { i++; j++; }
      shown += print_context_run(b, bo + dj, j - dj, ! changed, i == an && j == bn, dim, reset, emit);
      continue;
    }
    int di = i, aj = j;
    while (i < an || j < bn) {
      if (i < an && j < bn && line_eq(&a[ao + i], &b[bo + j])) break;
      if (j >= bn || (i < an && dp[(i + 1) * cols + j] >= dp[i * cols + j + 1])) i++;
      else j++;
    }
    changed = 1;
    shown += print_changed_hunk(a, ao + di, i - di, b, bo + aj, j - aj, red, green, dim, reset, emit, progress);
  }
  return shown + print_context_run(a, ao + an, suf, 0, 1, dim, reset, emit);
}

void print_lcs_diff(bytes_t * old, bytes_t * new, const char * red, const char * green, const char * dim, const char * reset, int full) {
  line_t * a = NULL, * b = NULL;
  int an = split_lines(old, &a), bn = split_lines(new, &b);
  int pre = 0, suf = 0;
  while (pre < an && pre < bn && line_eq(&a[pre], &b[pre])) pre++;
  while (suf + pre < an && suf + pre < bn && line_eq(&a[an - 1 - suf], &b[bn - 1 - suf])) suf++;
  int ao = pre, bo = pre;
  an -= pre + suf;
  bn -= pre + suf;
  long long cells = (long long) (an + 1) * (long long) (bn + 1);
  if (an < 0 || bn < 0 || (! full && cells > DIFF_CELL_LIMIT)) {
    if (pre) print_ellipsis(dim, reset);
    if (an < 0 || bn < 0) printf("  text diff unavailable\n");
    else printf("  text diff omitted (comparison too large to summarize; use --full)\n");
    if (suf) print_ellipsis(dim, reset);
    free(a); free(b);
    return;
  }
  int cols = bn + 1;
  int * dp = calloc((an + 1) * (bn + 1), sizeof(int));
  if (! dp) {
    if (full) {
      if (pre) print_ellipsis(dim, reset);
      for (int i = 0; i < an; i++) print_diff_line(red, '-', &a[ao + i], reset);
      for (int j = 0; j < bn; j++) print_diff_line(green, '+', &b[bo + j], reset);
      if (suf) print_ellipsis(dim, reset);
    }
    free(a); free(b);
    return;
  }
  progress_t progress;
  progress_start_unit(&progress, "diff", "lines");
  for (int i = an - 1; i >= 0; i--) {
    progress_tick(&progress, -1, NULL);
    for (int j = bn - 1; j >= 0; j--)
      dp[i * cols + j] = line_eq(&a[ao + i], &b[bo + j]) ? 1 + dp[(i + 1) * cols + j + 1] :
        (dp[(i + 1) * cols + j] > dp[i * cols + j + 1] ? dp[(i + 1) * cols + j] : dp[i * cols + j + 1]);
  }
  int shown_lines = render_lcs_diff(a, ao, an, b, bo, bn, pre, suf, dp, cols, red, green, dim, reset, 0, &progress);
  if (! full && shown_lines > DIFF_LINE_LIMIT) {
    if (pre) print_ellipsis(dim, reset);
    printf("  text diff omitted (would show %d diff lines; use --full)\n", shown_lines);
    if (suf) print_ellipsis(dim, reset);
    free(dp);
    free(a);
    free(b);
    progress_done(&progress);
    return;
  }
  render_lcs_diff(a, ao, an, b, bo, bn, pre, suf, dp, cols, red, green, dim, reset, 1, &progress);
  progress_done(&progress);
  free(dp);
  free(a);
  free(b);
}

int print_one_diff(const char * path, bytes_t * old, int old_type, int old_exists, bytes_t * new, int new_type, int new_exists, int * shown, int full) {
  if (bytes_equal(old, old_type, old_exists, new, new_type, new_exists)) return 0;
  int color = isatty(STDOUT_FILENO);
  const char * red = color ? "\033[31m" : "";
  const char * green = color ? "\033[32m" : "";
  const char * cyan = color ? "\033[36m" : "";
  const char * dim = color ? "\033[2m" : "";
  const char * reset = color ? "\033[0m" : "";
  const char * kind = ! old_exists && new_exists ? "added" :
    (old_exists && ! new_exists ? "deleted" :
      (old_type != new_type ? "type-change" : (old_type == SOURCE_LINK ? "link-change" : "modified")));
  printf("%s------------------------------------------------------------%s\n", dim, reset);
  printf("%s%s (%s)%s\n", cyan, path, kind, reset);
  (*shown)++;
  if (old_exists && new_exists && old_type != new_type) {
    printf("  source type: %s -> %s\n", old_type == SOURCE_LINK ? "link" : "file", new_type == SOURCE_LINK ? "link" : "file");
  } else if (old_type == SOURCE_LINK || new_type == SOURCE_LINK) {
    if (old_exists) { line_t l = {old->data, old->n}; print_diff_line(red, '-', &l, reset); }
    if (new_exists) { line_t l = {new->data, new->n}; print_diff_line(green, '+', &l, reset); }
  } else if ((old_exists && binary_like(old)) || (new_exists && binary_like(new))) {
    printf("  binary/source summary: %s%zu bytes -> %zu bytes%s\n", dim, old->n, new->n, reset);
  } else if (! old_exists) print_simple_lines(new, '+', green, reset, full);
  else if (! new_exists) print_simple_lines(old, '-', red, reset, full);
  else print_lcs_diff(old, new, red, green, dim, reset, full);
  return 1;
}

int print_move_diff(const char * old_path, const char * new_path, bytes_t * old, int old_type, int old_exists, bytes_t * new, int new_type, int new_exists, int * shown, int full) {
  int color = isatty(STDOUT_FILENO);
  const char * red = color ? "\033[31m" : "";
  const char * green = color ? "\033[32m" : "";
  const char * cyan = color ? "\033[36m" : "";
  const char * dim = color ? "\033[2m" : "";
  const char * reset = color ? "\033[0m" : "";
  printf("%s------------------------------------------------------------%s\n", dim, reset);
  printf("%s%s -> %s (moved)%s\n", cyan, old_path, new_path, reset);
  (*shown)++;
  if (bytes_equal(old, old_type, old_exists, new, new_type, new_exists)) return 1;
  if (old_exists && new_exists && old_type != new_type) {
    printf("  source type: %s -> %s\n", old_type == SOURCE_LINK ? "link" : "file", new_type == SOURCE_LINK ? "link" : "file");
  } else if (old_type == SOURCE_LINK || new_type == SOURCE_LINK) {
    if (old_exists) { line_t l = {old->data, old->n}; print_diff_line(red, '-', &l, reset); }
    if (new_exists) { line_t l = {new->data, new->n}; print_diff_line(green, '+', &l, reset); }
  } else if ((old_exists && binary_like(old)) || (new_exists && binary_like(new))) {
    printf("  binary/source summary: %s%zu bytes -> %zu bytes%s\n", dim, old->n, new->n, reset);
  } else print_lcs_diff(old, new, red, green, dim, reset, full);
  return 1;
}

void current_work_state(repo_t * repo, index_t * idx, const char * path, bytes_t * b, int * type, int * exists) {
  char full[PATH_MAX];
  int j = index_find(idx, path);
  *type = SOURCE_NONE;
  *exists = 0;
  empty_bytes(b);
  if (j < 0 || idx->v[j].removed) return;
  path_join(full, sizeof(full), repo->root, path);
  free_bytes(b);
  if (read_source(full, b, type)) *exists = 1;
  else empty_bytes(b);
}

int committed_base_state(repo_t * repo, index_entry_t * e, bytes_t * b, int * type, int * exists) {
  char cache[PATH_MAX];
  *type = e->type;
  *exists = 0;
  cache_path(repo, e->path, cache);
  if (! read_bytes(cache, b)) { empty_bytes(b); return b->data != NULL; }
  *exists = 1;
  return 1;
}

int diff_local(repo_t * repo, index_t * idx, stage_t * stage, const char * filter, int full) {
  int shown = 0;
  paths_t work_paths = {0};
  int * move_path = malloc((idx->n ? idx->n : 1) * sizeof(int));
  int * skip_path = NULL;
  if (move_path) for (int i = 0; i < idx->n; i++) move_path[i] = -1;
  progress_t scan;
  progress_start(&scan, "scan");
  scan_path(repo, "", &work_paths, &scan);
  progress_done(&scan);
  qsort(work_paths.v, work_paths.n, sizeof(char*), cmp_path_ptrs);
  skip_path = calloc(work_paths.n ? work_paths.n : 1, sizeof(int));
  if (move_path && skip_path) pair_work_moves(repo, idx, stage, &work_paths, move_path, skip_path);
  progress_t progress;
  progress_start(&progress, "diff");
  for (int i = 0; i < idx->n; i++) {
    index_entry_t * e = &idx->v[i];
    int move_si = stage_find_move_from(stage, e->path);
    stage_entry_t * move = move_si >= 0 ? &stage->v[move_si] : NULL;
    int work_move = move_path ? move_path[i] : -1;
    if (filter && strcmp(filter, e->path) != 0 && (! move || strcmp(filter, move->path) != 0) &&
        (work_move < 0 || strcmp(filter, work_paths.v[work_move]) != 0)) continue;
    progress_tick(&progress, filter ? -1 : idx->n, e->path);
    bytes_t old = {0}, new = {0};
    int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0;
    int si = stage_find(stage, e->path);
    if (work_move >= 0) {
      char moved_full[PATH_MAX];
      committed_base_state(repo, e, &old, &old_type, &old_exists);
      path_join(moved_full, sizeof(moved_full), repo->root, work_paths.v[work_move]);
      if (read_source(moved_full, &new, &new_type)) new_exists = 1;
      else empty_bytes(&new);
      print_move_diff(e->path, work_paths.v[work_move], &old, old_type, old_exists, &new, new_type, new_exists, &shown, full);
      free_bytes(&old);
      free_bytes(&new);
      continue;
    }
    if (move) {
      char moved_full[PATH_MAX];
      read_staged(repo, move, &old, &old_type, &old_exists);
      path_join(moved_full, sizeof(moved_full), repo->root, move->path);
      if (read_source(moved_full, &new, &new_type)) new_exists = 1;
      else empty_bytes(&new);
      print_one_diff(move->path, &old, old_type, old_exists, &new, new_type, new_exists, &shown, full);
      free_bytes(&old);
      free_bytes(&new);
      continue;
    }
    if (si >= 0) read_staged(repo, &stage->v[si], &old, &old_type, &old_exists);
    else committed_base_state(repo, e, &old, &old_type, &old_exists);
    current_work_state(repo, idx, e->path, &new, &new_type, &new_exists);
    print_one_diff(e->path, &old, old_type, old_exists, &new, new_type, new_exists, &shown, full);
    free_bytes(&old);
    free_bytes(&new);
  }
  progress_done(&progress);
  free(move_path);
  free(skip_path);
  paths_free(&work_paths);
  return shown;
}

int diff_staged(repo_t * repo, index_t * idx, stage_t * stage, const char * filter, int full) {
  int shown = 0;
  progress_t progress;
  progress_start(&progress, "diff");
  for (int i = 0; i < stage->n; i++) {
    stage_entry_t * se = &stage->v[i];
    if (filter && strcmp(filter, se->path) != 0 && (! se->old_path || strcmp(filter, se->old_path) != 0)) continue;
    progress_tick(&progress, filter ? -1 : stage->n, se->path);
    int j = index_find(idx, se->action == STAGE_MOVE && se->old_path ? se->old_path : se->path);
    index_entry_t base = (j >= 0) ? idx->v[j] : (index_entry_t){se->path_hash, 0, 0, 0, SOURCE_NONE, 0, se->path};
    bytes_t old = {0}, new = {0};
    int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0;
    committed_base_state(repo, &base, &old, &old_type, &old_exists);
    read_staged(repo, se, &new, &new_type, &new_exists);
    if (se->action == STAGE_MOVE && se->old_path)
      print_move_diff(se->old_path, se->path, &old, old_type, old_exists, &new, new_type, new_exists, &shown, full);
    else
      print_one_diff(se->path, &old, old_type, old_exists, &new, new_type, new_exists, &shown, full);
    free_bytes(&old);
    free_bytes(&new);
  }
  progress_done(&progress);
  return shown;
}

int diff_against(repo_t * repo, index_t * idx, unsigned long long target, const char * filter, int full) {
  paths_t paths = {0};
  int shown = 0;
  int ok = 1;
  for (int i = 0; ok && i < idx->n; i++)
    ok = paths_push_unique(&paths, idx->v[i].path);
  for (unsigned long long h = read_head(repo); ok && target != read_head(repo) && h && h != target;) {
    commit_t c = {0};
    if (! read_commit(repo, h, &c)) { ok = 0; break; }
    for (int i = 0; ok && i < c.n; i++)
      ok = paths_push_commit_file(&paths, &c.files[i]);
    h = c.parent;
    commit_free(&c);
    if (h == target) break;
    if (! h && target != 0) ok = 0;
  }
  if (! ok) {
    fprintf(stderr, "unknown commit: %llu\n", target);
    return 1;
  }
  progress_t progress;
  progress_start(&progress, "diff");
  for (int i = 0; i < paths.n; i++) {
    if (filter && strcmp(filter, paths.v[i]) != 0) continue;
    progress_tick(&progress, filter ? -1 : paths.n, paths.v[i]);
    bytes_t old = {0}, new = {0};
    int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0;
    if (! state_at_commit(repo, idx, paths.v[i], target, &old, &old_type, &old_exists)) continue;
    current_work_state(repo, idx, paths.v[i], &new, &new_type, &new_exists);
    print_one_diff(paths.v[i], &old, old_type, old_exists, &new, new_type, new_exists, &shown, full);
    free_bytes(&old);
    free_bytes(&new);
  }
  progress_done(&progress);
  paths_free(&paths);
  return 0;
}

int diff_commit_ref(repo_t * repo, const char * s, unsigned long long * id, int allow_root) {
  if (strcmp(s, "HEAD") == 0) return read_head_id(repo, id);
  char * end = NULL;
  *id = strtoull(s, &end, 10);
  return end && ! *end && ((*id == 0 && allow_root) || commit_exists(repo, *id));
}

void normalize_filter_path(char * s) {
  while (strncmp(s, "./", 2) == 0) memmove(s, s + 2, strlen(s + 2) + 1);
}

int collect_paths_between(repo_t * repo, index_t * idx, unsigned long long base, unsigned long long target, paths_t * paths) {
  for (int i = 0; i < idx->n; i++)
    if (! paths_push_unique(paths, idx->v[i].path)) return 0;
  for (unsigned long long h = target;;) {
    commit_t c = {0};
    if (! read_commit(repo, h, &c)) return 0;
    for (int i = 0; i < c.n; i++)
      if (! paths_push_commit_file(paths, &c.files[i])) { commit_free(&c); return 0; }
    unsigned long long parent = c.parent;
    commit_free(&c);
    if (h == base) return 1;
    if (h == 0 || (parent == 0 && ! commit_exists(repo, 0))) return base == 0;
    h = parent;
  }
}

int diff_commits(repo_t * repo, index_t * idx, unsigned long long base, unsigned long long target, const char * filter, int full, int base_root) {
  paths_t paths = {0};
  int shown = 0;
  if (! collect_paths_between(repo, idx, base, target, &paths)) {
    fprintf(stderr, "invalid commit range: %llu is not an ancestor of %llu\n", base, target);
    return 1;
  }
  progress_t progress;
  progress_start(&progress, "diff");
  for (int i = 0; i < paths.n; i++) {
    if (filter && strcmp(filter, paths.v[i]) != 0) continue;
    progress_tick(&progress, filter ? -1 : paths.n, paths.v[i]);
    bytes_t old = {0}, new = {0};
    int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0;
    if (base_root) empty_bytes(&old);
    else if (! state_at_commit(repo, idx, paths.v[i], base, &old, &old_type, &old_exists)) continue;
    if (! state_at_commit(repo, idx, paths.v[i], target, &new, &new_type, &new_exists)) { free_bytes(&old); continue; }
    print_one_diff(paths.v[i], &old, old_type, old_exists, &new, new_type, new_exists, &shown, full);
    free_bytes(&old);
    free_bytes(&new);
  }
  progress_done(&progress);
  paths_free(&paths);
  return 0;
}

int diff_latest(repo_t * repo, const char * filter, int full) {
  unsigned long long head = 0;
  commit_t c = {0};
  int shown = 0;
  if (! read_head_id(repo, &head) || ! read_commit(repo, head, &c)) return 1;
  progress_t progress;
  progress_start(&progress, "diff");
  for (int i = 0; i < c.n; i++) {
    commit_file_t * f = &c.files[i];
    if (filter && strcmp(filter, f->path) != 0) continue;
    progress_tick(&progress, filter ? -1 : c.n, f->path);
    bytes_t old = {0}, new = {0};
    int old_type = f->old_type, old_exists = f->kind != 1;
    int new_type = f->new_type, new_exists = f->kind != 2;
    if (new_exists) {
      char cache[PATH_MAX];
      cache_path(repo, f->path, cache);
      if (! read_bytes(cache, &new)) empty_bytes(&new);
    } else empty_bytes(&new);
    bytes_copy(&old, &new);
    reverse_state(f, &old, &old_type, &old_exists);
    print_one_diff(f->path, &old, old_type, old_exists, &new, new_type, new_exists, &shown, full);
    free_bytes(&old);
    free_bytes(&new);
  }
  progress_done(&progress);
  commit_free(&c);
  return 0;
}

int command_diff(repo_t * repo, int argc, char ** argv) {
  index_t idx = {0};
  stage_t stage = {0};
  char filter_buf[PATH_MAX], * filter = NULL;
  int latest = 0, against_set = 0, full = 0, staged = 0, target_set = 0;
  unsigned long long against = 0, target = 0;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--latest") == 0) latest = 1;
    else if (strcmp(argv[i], "--full") == 0) full = 1;
    else if (strcmp(argv[i], "--staged") == 0) staged = 1;
    else if (strcmp(argv[i], "--against") == 0) {
      if (i + 1 >= argc) { fprintf(stderr, "usage: sc diff [--full] [--staged|--latest] [path|COMMIT] [--against COMMIT]\n"); return 1; }
      against_set = 1;
      if (! diff_commit_ref(repo, argv[++i], &against, 1)) { fprintf(stderr, "unknown commit: %s\n", argv[i]); return 1; }
    } else if (! filter) {
      if (argv[i][0] == '-') { fprintf(stderr, "usage: sc diff [--full] [--staged|--latest] [path|COMMIT] [--against COMMIT]\n"); return 1; }
      if (! target_set && diff_commit_ref(repo, argv[i], &target, 0)) target_set = 1;
      else {
        int commitish = strcmp(argv[i], "HEAD") == 0;
        if (! commitish) {
          commitish = argv[i][0] != '\0';
          for (int j = 0; commitish && argv[i][j]; j++)
            if (argv[i][j] < '0' || argv[i][j] > '9') commitish = 0;
        }
        if (commitish) { fprintf(stderr, "unknown commit: %s\n", argv[i]); return 1; }
      }
      if (! target_set && ! filter) {
        if (rel_path(repo, argv[i], filter_buf)) { normalize_filter_path(filter_buf); filter = filter_buf; }
        else { snprintf(filter_buf, sizeof(filter_buf), "%s", argv[i]); normalize_filter_path(filter_buf); filter = filter_buf; }
      }
    } else {
      if (! target_set && diff_commit_ref(repo, argv[i], &target, 0)) target_set = 1;
      else {
        fprintf(stderr, "usage: sc diff [--full] [--staged|--latest] [path|COMMIT] [--against COMMIT]\n");
        return 1;
      }
    }
  }
  if ((latest && (against_set || target_set)) || (staged && (latest || against_set || target_set))) {
    fprintf(stderr, "usage: sc diff [--full] [--staged|--latest] [path|COMMIT] [--against COMMIT]\n");
    return 1;
  }
  int base_root = 0;
  if (target_set && ! against_set) {
    commit_t c = {0};
    if (! read_commit(repo, target, &c)) { fprintf(stderr, "unknown commit: %llu\n", target); return 1; }
    against = c.parent;
    base_root = target == 0 || (c.parent == 0 && ! commit_exists(repo, 0));
    commit_free(&c);
    against_set = 1;
  } else if (target_set && against == 0 && ! commit_exists(repo, 0)) {
    base_root = 1;
  }
  if (! load_index(repo, &idx)) return 1;
  if (! load_stage(repo, &stage)) return 1;
  int rc = latest ? diff_latest(repo, filter, full) :
    (target_set ? diff_commits(repo, &idx, against, target, filter, full, base_root) :
      (against_set ? diff_against(repo, &idx, against, filter, full) : 0));
  if (staged) diff_staged(repo, &idx, &stage, filter, full);
  else if (! latest && ! against_set && ! diff_local(repo, &idx, &stage, filter, full) && stage.n)
    diff_staged(repo, &idx, &stage, filter, full);
  stage_free(&stage);
  index_free(&idx);
  return rc;
}

int read_chain(repo_t * repo, ids_t * chain) {
  unsigned long long h = 0;
  if (! read_head_id(repo, &h)) return 1;
  while (1) {
    commit_t c = {0};
    if (! read_commit(repo, h, &c) || ! ids_push(chain, h)) { commit_free(&c); return 0; }
    if (h == 0 || c.parent != 0 || ! commit_exists(repo, 0)) {
      h = c.parent;
      commit_free(&c);
      if (! h) break;
      continue;
    }
    h = c.parent;
    commit_free(&c);
  }
  return 1;
}

int parse_commit_ref(repo_t * repo, const char * s, unsigned long long * id) {
  if (strcmp(s, "HEAD") == 0) return read_head_id(repo, id);
  char * end = NULL;
  *id = strtoull(s, &end, 10);
  return end && ! *end && (*id != 0 || commit_exists(repo, 0));
}

int resolve_squash(repo_t * repo, ids_t * chain, const char * arg, unsigned long long * start, unsigned long long * end, unsigned long long * keep, int * lone) {
  char buf[256];
  char * dots = strstr(arg, "..");
  *lone = 0;
  if (dots) {
    size_t n = (size_t) (dots - arg);
    if (n >= sizeof(buf)) return 0;
    memcpy(buf, arg, n);
    buf[n] = '\0';
    if (! parse_commit_ref(repo, buf, start) || ! parse_commit_ref(repo, dots + 2, end)) return 0;
    *keep = *end;
  } else {
    unsigned long long id = 0;
    if (! parse_commit_ref(repo, arg, &id)) return 0;
    int pos = ids_find(chain, id);
    if (pos < 0) return 0;
    if (chain->n == 1) { *start = *end = id; *lone = 1; return 1; }
    if (pos > 0) { *start = id; *end = chain->v[pos - 1]; *keep = *end; return 1; }
    *start = chain->v[1];
    *end = id;
    *keep = strcmp(arg, "HEAD") == 0 ? *start : *end;
    return 1;
  }
  int si = ids_find(chain, *start), ei = ids_find(chain, *end);
  return si >= 0 && ei >= 0 && si >= ei;
}

int repo_clean(repo_t * repo, const char * action) {
  index_t idx = {0};
  stage_t stage = {0};
  if (! load_index(repo, &idx) || ! load_stage(repo, &stage)) return 0;
  if (stage.n) { fprintf(stderr, "%s requires clean staged changes\n", action); stage_free(&stage); index_free(&idx); return 0; }
  progress_t progress;
  progress_start(&progress, action);
  for (int i = 0; i < idx.n; i++) {
    progress_tick(&progress, idx.n, idx.v[i].path);
    bytes_t old = {0}, now = {0};
    int old_type = SOURCE_NONE, now_type = SOURCE_NONE, old_exists = 0, now_exists = 0;
    committed_base_state(repo, &idx.v[i], &old, &old_type, &old_exists);
    current_work_state(repo, &idx, idx.v[i].path, &now, &now_type, &now_exists);
    int clean = bytes_equal(&old, old_type, old_exists, &now, now_type, now_exists);
    free_bytes(&old);
    free_bytes(&now);
    if (! clean) { progress_done(&progress); fprintf(stderr, "%s requires clean working tree\n", action); stage_free(&stage); index_free(&idx); return 0; }
  }
  progress_done(&progress);
  stage_free(&stage);
  index_free(&idx);
  return 1;
}

unsigned long long next_commit_id(repo_t * repo) {
  char cdir[PATH_MAX];
  unsigned long long id = 0, max = 0;
  path_join(cdir, sizeof(cdir), repo->sc, "commits");
  DIR * dir = opendir(cdir);
  if (! dir) return read_head(repo) + 1;
  struct dirent * de;
  while ((de = readdir(dir)))
    if (sscanf(de->d_name, "%llx.sc", &id) == 1 && id > max) max = id;
  closedir(dir);
  return max + 1;
}

int target_state(repo_t * repo, index_t * idx, const char * path, unsigned long long target, int root, bytes_t * b, int * type, int * exists) {
  if (! root) return state_at_commit(repo, idx, path, target, b, type, exists);
  *type = SOURCE_NONE;
  *exists = 0;
  empty_bytes(b);
  return b->data != NULL;
}

int collect_restore_paths(repo_t * repo, index_t * idx, unsigned long long target, int target_root, paths_t * paths) {
  unsigned long long h = 0;
  if (! read_head_id(repo, &h)) return 0;
  for (int i = 0; i < idx->n; i++)
    if (! paths_push_unique(paths, idx->v[i].path)) return 0;
  while (1) {
    commit_t c = {0};
    if (! read_commit(repo, h, &c)) return 0;
    for (int i = 0; i < c.n; i++)
      if (! paths_push_commit_file(paths, &c.files[i])) { commit_free(&c); return 0; }
    unsigned long long parent = c.parent;
    int done = target_root ? (h == 0 || (parent == 0 && ! commit_exists(repo, 0))) : h == target;
    commit_free(&c);
    if (done) return 1;
    if (h == 0 || (parent == 0 && ! commit_exists(repo, 0))) return 0;
    h = parent;
  }
}

int write_restore_commit(repo_t * repo, index_t * idx, paths_t * paths, unsigned long long id, unsigned long long head, unsigned long long target, int target_root, long long t, char * msg) {
  char body[PATH_MAX], outp[PATH_MAX], cpath[PATH_MAX], buf[8192];
  snprintf(body, sizeof(body), "%s/commits/%016llx.body", repo->sc, id);
  snprintf(outp, sizeof(outp), "%s/commits/%016llx.tmp", repo->sc, id);
  snprintf(cpath, sizeof(cpath), "%s/commits/%016llx.sc", repo->sc, id);
  FILE * body_fp = fopen(body, "wb");
  if (! body_fp) return -1;
  int changed = 0;
  for (int i = 0; i < paths->n; i++) {
    bytes_t old = {0}, new = {0};
    edits_t edits = {0};
    int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0, kind = 0;
    if (! state_at_commit(repo, idx, paths->v[i], head, &old, &old_type, &old_exists) ||
        ! target_state(repo, idx, paths->v[i], target, target_root, &new, &new_type, &new_exists)) {
      fclose(body_fp);
      unlink(body);
      free_bytes(&old);
      free_bytes(&new);
      return -1;
    }
    if (! bytes_equal(&old, old_type, old_exists, &new, new_type, new_exists)) {
      if (! old_exists && new_exists) kind = 1;
      else if (old_exists && ! new_exists) kind = 2;
      if (! build_reverse_edits(kind, &old, &new, &edits)) {
        fclose(body_fp);
        unlink(body);
        free_bytes(&old);
        free_bytes(&new);
        return -1;
      }
      write_commit_file(body_fp, paths->v[i], kind, old_type, new_type, &old, &new, &edits);
      changed++;
    }
    edits_free(&edits);
    free_bytes(&old);
    free_bytes(&new);
  }
  fclose(body_fp);
  if (! changed) { unlink(body); return 0; }
  FILE * out = fopen(outp, "wb");
  FILE * in = fopen(body, "rb");
  if (! out || ! in) {
    if (out) fclose(out);
    if (in) fclose(in);
    unlink(body);
    unlink(outp);
    return -1;
  }
  fprintf(out, "SC1\ncommit %llu\nparent %llu\ntime %lld\nmessage %zu\n", id, head, t, strlen(msg));
  fwrite(msg, 1, strlen(msg), out);
  fprintf(out, "\nfiles %d\n", changed);
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in))) fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  unlink(body);
  return rename(outp, cpath) == 0 ? changed : -1;
}

int apply_restore_state(repo_t * repo, index_t * idx, paths_t * paths, unsigned long long target, int target_root) {
  bytes_t * states = calloc(paths->n ? paths->n : 1, sizeof(bytes_t));
  int * types = calloc(paths->n ? paths->n : 1, sizeof(int));
  int * exists = calloc(paths->n ? paths->n : 1, sizeof(int));
  if (! states || ! types || ! exists) { free(states); free(types); free(exists); return 0; }
  for (int i = 0; i < paths->n; i++) {
    char full[PATH_MAX];
    bytes_t old = {0};
    int old_type = SOURCE_NONE, old_exists = 0;
    path_join(full, sizeof(full), repo->root, paths->v[i]);
    current_work_state(repo, idx, paths->v[i], &old, &old_type, &old_exists);
    if (! target_state(repo, idx, paths->v[i], target, target_root, &states[i], &types[i], &exists[i])) {
      free_bytes(&old);
      goto fail;
    }
    if (! old_exists && exists[i]) {
      struct stat st;
      if (lstat(full, &st) == 0) {
        fprintf(stderr, "restore would overwrite untracked: %s\n", paths->v[i]);
        free_bytes(&old);
        goto fail;
      }
    }
    free_bytes(&old);
  }
  for (int i = 0; i < paths->n; i++) {
    char full[PATH_MAX], cache[PATH_MAX];
    path_join(full, sizeof(full), repo->root, paths->v[i]);
    cache_path(repo, paths->v[i], cache);
    int j = index_find(idx, paths->v[i]);
    if (exists[i]) {
      struct stat st;
      if (! write_source(full, &states[i], types[i]) || ! write_bytes(cache, states[i].data, states[i].n) || lstat(full, &st) != 0) goto fail;
      index_entry_t e = {hash_string(paths->v[i]), hash_bytes(states[i].data, states[i].n), states[i].n, st.st_mtime, types[i], 0, xstrdup(paths->v[i])};
      if (! e.path) goto fail;
      if (j >= 0) { free(idx->v[j].path); idx->v[j] = e; }
      else if (! index_push(idx, e)) {
        free(e.path);
        goto fail;
      }
    } else {
      unlink(full);
      unlink(cache);
      if (j >= 0) {
        free(idx->v[j].path);
        memmove(idx->v + j, idx->v + j + 1, (idx->n - j - 1) * sizeof(index_entry_t));
        idx->n--;
      }
    }
  }
  int ok = save_index(repo, idx);
  for (int i = 0; i < paths->n; i++) free_bytes(&states[i]);
  free(states);
  free(types);
  free(exists);
  return ok;
fail:
  for (int i = 0; i < paths->n; i++) free_bytes(&states[i]);
  free(states);
  free(types);
  free(exists);
  return 0;
}

int restore_to(repo_t * repo, unsigned long long target, int target_root, const char * action, char * msg) {
  unsigned long long head = 0;
  if (! read_head_id(repo, &head)) { printf("at root\n"); return 0; }
  if (! repo_clean(repo, action)) return 1;
  index_t idx = {0};
  paths_t paths = {0};
  int rc = 1;
  if (! load_index(repo, &idx) || ! collect_restore_paths(repo, &idx, target, target_root, &paths)) goto done;
  unsigned long long id = next_commit_id(repo);
  long long now = (long long) time(NULL);
  int changed = write_restore_commit(repo, &idx, &paths, id, head, target, target_root, now, msg);
  if (changed < 0) goto done;
  if (! changed) { printf("nothing to restore\n"); rc = 0; goto done; }
  if (! apply_restore_state(repo, &idx, &paths, target, target_root)) {
    char cpath[PATH_MAX];
    snprintf(cpath, sizeof(cpath), "%s/commits/%016llx.sc", repo->sc, id);
    unlink(cpath);
    goto done;
  }
  stage_t stage = {0};
  save_stage(repo, &stage);
  write_head(repo, id);
  print_action_msg("commit", id, now, msg);
  rc = 0;
done:
  paths_free(&paths);
  index_free(&idx);
  return rc;
}

int command_restore(repo_t * repo, int argc, char ** argv) {
  if (argc != 1) { fprintf(stderr, "usage: sc restore COMMIT\n"); return 1; }
  ids_t chain = {0};
  unsigned long long target = 0;
  int ok = parse_commit_ref(repo, argv[0], &target) && read_chain(repo, &chain) && ids_find(&chain, target) >= 0;
  ids_free(&chain);
  if (! ok) { fprintf(stderr, "unknown commit: %s\n", argv[0]); return 1; }
  char msg[64];
  snprintf(msg, sizeof(msg), "restore %llu", target);
  return restore_to(repo, target, 0, "restore", msg);
}

int range_ids(repo_t * repo, unsigned long long start, unsigned long long end, ids_t * ids) {
  for (unsigned long long h = end;;) {
    commit_t c = {0};
    if (! read_commit(repo, h, &c) || ! ids_push(ids, h)) { commit_free(&c); return 0; }
    unsigned long long parent = c.parent;
    commit_free(&c);
    if (h == start) return 1;
    if (h == 0 || (parent == 0 && ! commit_exists(repo, 0))) return 0;
    h = parent;
  }
}

int range_paths(repo_t * repo, ids_t * ids, paths_t * paths) {
  for (int i = 0; i < ids->n; i++) {
    commit_t c = {0};
    if (! read_commit(repo, ids->v[i], &c)) return 0;
    for (int j = 0; j < c.n; j++)
      if (! paths_push_commit_file(paths, &c.files[j])) { commit_free(&c); return 0; }
    commit_free(&c);
  }
  return 1;
}

int append_bytes(char ** s, size_t * n, size_t * cap, const char * add, size_t add_n) {
  if (*n + add_n + 1 > *cap) {
    while (*n + add_n + 1 > *cap) *cap = *cap ? 2 * *cap : 256;
    char * v = realloc(*s, *cap);
    if (! v) return 0;
    *s = v;
  }
  memcpy(*s + *n, add, add_n);
  *n += add_n;
  (*s)[*n] = '\0';
  return 1;
}

int build_squash_message(repo_t * repo, ids_t * ids, char ** out) {
  char * msg = NULL, tag[64];
  size_t n = 0, cap = 0;
  for (int i = ids->n - 1; i >= 0; i--) {
    commit_t c = {0};
    if (! read_commit(repo, ids->v[i], &c)) { free(msg); return 0; }
    size_t m = strlen(c.msg ? c.msg : "");
    while (m && (c.msg[m - 1] == '\n' || c.msg[m - 1] == '\r')) m--;
    snprintf(tag, sizeof(tag), " [%llu]", ids->v[i]);
    int ok = (n == 0 || append_bytes(&msg, &n, &cap, "\n", 1)) &&
      append_bytes(&msg, &n, &cap, c.msg ? c.msg : "", m) &&
      append_bytes(&msg, &n, &cap, tag, strlen(tag));
    commit_free(&c);
    if (! ok) { free(msg); return 0; }
  }
  *out = msg ? msg : xstrdup("");
  return *out != NULL;
}

void commit_path(repo_t * repo, unsigned long long id, char * path) {
  snprintf(path, PATH_MAX, "%s/commits/%016llx.sc", repo->sc, id);
}

int commit_exists(repo_t * repo, unsigned long long id) {
  char path[PATH_MAX];
  commit_path(repo, id, path);
  return is_file(path);
}

void unlink_commit(repo_t * repo, unsigned long long id) {
  char path[PATH_MAX];
  commit_path(repo, id, path);
  unlink(path);
}

int write_squashed_commit(repo_t * repo, index_t * idx, unsigned long long id, unsigned long long state_id, unsigned long long parent, long long t, char * msg, paths_t * paths, int root_old) {
  char body[PATH_MAX], outp[PATH_MAX], cpath[PATH_MAX], buf[8192];
  snprintf(body, sizeof(body), "%s/commits/%016llx.body", repo->sc, id);
  snprintf(outp, sizeof(outp), "%s/commits/%016llx.tmp", repo->sc, id);
  commit_path(repo, id, cpath);
  FILE * body_fp = fopen(body, "wb");
  if (! body_fp) return 0;
  int changed = 0;
  for (int i = 0; i < paths->n; i++) {
    bytes_t old = {0}, new = {0};
    edits_t edits = {0};
    int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0, kind = 0;
    if (root_old) empty_bytes(&old);
    else if (! state_at_commit(repo, idx, paths->v[i], parent, &old, &old_type, &old_exists)) {
      fclose(body_fp);
      return 0;
    }
    if (! state_at_commit(repo, idx, paths->v[i], state_id, &new, &new_type, &new_exists)) { fclose(body_fp); return 0; }
    if (! bytes_equal(&old, old_type, old_exists, &new, new_type, new_exists)) {
      if (! old_exists && new_exists) kind = 1;
      else if (old_exists && ! new_exists) kind = 2;
      if (! build_reverse_edits(kind, &old, &new, &edits)) { fclose(body_fp); return 0; }
      write_commit_file(body_fp, paths->v[i], kind, old_type, new_type, &old, &new, &edits);
      changed++;
    }
    edits_free(&edits);
    free_bytes(&old);
    free_bytes(&new);
  }
  fclose(body_fp);
  FILE * out = fopen(outp, "wb");
  FILE * in = fopen(body, "rb");
  if (! out || ! in) return 0;
  fprintf(out, "SC1\ncommit %llu\nparent %llu\ntime %lld\nmessage %zu\n", id, parent, t, strlen(msg));
  fwrite(msg, 1, strlen(msg), out);
  fprintf(out, "\nfiles %d\n", changed);
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in))) fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  unlink(body);
  return rename(outp, cpath) == 0;
}

int write_amended_commit(repo_t * repo, index_t * idx, stage_t * stage, commit_t * c, char * msg, paths_t * paths) {
  char body[PATH_MAX], outp[PATH_MAX], cpath[PATH_MAX], buf[8192];
  int root_old = c->id == 0 || (c->parent == 0 && ! commit_exists(repo, 0));
  snprintf(body, sizeof(body), "%s/commits/%016llx.body", repo->sc, c->id);
  snprintf(outp, sizeof(outp), "%s/commits/%016llx.tmp", repo->sc, c->id);
  commit_path(repo, c->id, cpath);
  FILE * body_fp = fopen(body, "wb");
  if (! body_fp) return 0;
  int changed = 0;
  for (int i = 0; i < paths->n; i++) {
    bytes_t old = {0}, new = {0};
    edits_t edits = {0};
    int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0, kind = 0;
    int si = stage_find(stage, paths->v[i]);
    if (root_old) empty_bytes(&old);
    else if (! state_at_commit(repo, idx, paths->v[i], c->parent, &old, &old_type, &old_exists)) { fclose(body_fp); return 0; }
    if (si >= 0) read_staged(repo, &stage->v[si], &new, &new_type, &new_exists);
    else if (! state_at_commit(repo, idx, paths->v[i], c->id, &new, &new_type, &new_exists)) { fclose(body_fp); return 0; }
    if (! bytes_equal(&old, old_type, old_exists, &new, new_type, new_exists)) {
      if (! old_exists && new_exists) kind = 1;
      else if (old_exists && ! new_exists) kind = 2;
      if (! build_reverse_edits(kind, &old, &new, &edits)) { fclose(body_fp); return 0; }
      write_commit_file(body_fp, paths->v[i], kind, old_type, new_type, &old, &new, &edits);
      changed++;
    }
    edits_free(&edits);
    free_bytes(&old);
    free_bytes(&new);
  }
  fclose(body_fp);
  FILE * out = fopen(outp, "wb");
  FILE * in = fopen(body, "rb");
  if (! out || ! in) return 0;
  fprintf(out, "SC1\ncommit %llu\nparent %llu\ntime %lld\nmessage %zu\n", c->id, c->parent, c->time, strlen(msg));
  fwrite(msg, 1, strlen(msg), out);
  fprintf(out, "\nfiles %d\n", changed);
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in))) fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  unlink(body);
  return rename(outp, cpath) == 0;
}

int clear_dir_files(const char * dir) {
  DIR * d = opendir(dir);
  if (! d) return 1;
  struct dirent * de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.') continue;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
    unlink(path);
  }
  closedir(d);
  return 1;
}

int squash_lone(repo_t * repo) {
  index_t idx = {0}, next = {0};
  stage_t stage = {0};
  if (! load_index(repo, &idx)) return 1;
  char dir[PATH_MAX];
  path_join(dir, sizeof(dir), repo->sc, "commits"); clear_dir_files(dir);
  path_join(dir, sizeof(dir), repo->sc, "files"); clear_dir_files(dir);
  path_join(dir, sizeof(dir), repo->sc, "staged"); clear_dir_files(dir);
  for (int i = 0; i < idx.n; i++) {
    char full[PATH_MAX], spath[PATH_MAX];
    struct stat st;
    bytes_t b = {0};
    int type = SOURCE_NONE;
    path_join(full, sizeof(full), repo->root, idx.v[i].path);
    if (! read_source(full, &b, &type) || lstat(full, &st) != 0) continue;
    staged_path(repo, idx.v[i].path, spath);
    write_bytes(spath, b.data, b.n);
    index_entry_t ie = {hash_string(idx.v[i].path), 0, 0, 0, SOURCE_NONE, 0, xstrdup(idx.v[i].path)};
    stage_entry_t se = {hash_string(idx.v[i].path), hash_bytes(b.data, b.n), b.n, st.st_mtime, type, STAGE_ADD, NULL, xstrdup(idx.v[i].path)};
    index_push(&next, ie);
    stage_push(&stage, se);
    free_bytes(&b);
  }
  int ok = write_head(repo, 0) && save_index(repo, &next) && save_stage(repo, &stage);
  index_free(&idx);
  index_free(&next);
  stage_free(&stage);
  printf("squash cleared history\n");
  return ok ? 0 : 1;
}

int command_amend(repo_t * repo, int argc, char ** argv) {
  char * msg = NULL;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) msg = argv[++i];
    else { fprintf(stderr, "usage: sc amend [-m \"message\"]\n"); return 1; }
  }
  unsigned long long head = 0;
  if (! read_head_id(repo, &head)) { fprintf(stderr, "nothing to amend\n"); return 1; }
  index_t idx = {0};
  stage_t stage = {0};
  commit_t c = {0};
  paths_t paths = {0};
  int rc = 1;
  if (! load_index(repo, &idx) || ! load_stage(repo, &stage) || ! read_commit(repo, head, &c)) goto done;
  if (! stage.n) {
    int auto_n = stage_unstaged(repo, &idx, &stage);
    if (auto_n && (! save_index(repo, &idx) || ! save_stage(repo, &stage))) goto done;
  }
  if (! stage.n && ! msg) { printf("nothing to amend\n"); rc = 1; goto done; }
  for (int i = 0; i < c.n; i++)
    if (! paths_push_commit_file(&paths, &c.files[i])) goto done;
  for (int i = 0; i < stage.n; i++) {
    if (stage.v[i].old_path && ! paths_push_unique(&paths, stage.v[i].old_path)) goto done;
    if (! paths_push_unique(&paths, stage.v[i].path)) goto done;
  }
  if (! write_amended_commit(repo, &idx, &stage, &c, msg ? msg : (c.msg ? c.msg : ""), &paths)) goto done;
  for (int i = 0; i < stage.n; i++) {
    stage_entry_t * se = &stage.v[i];
    int j = index_find(&idx, se->path);
    char cache[PATH_MAX], spath[PATH_MAX];
    bytes_t b = {0};
    cache_path(repo, se->path, cache);
    staged_path(repo, se->path, spath);
    if (se->action == STAGE_MOVE && se->old_path) {
      char old_cache[PATH_MAX];
      int old_j = index_find(&idx, se->old_path);
      cache_path(repo, se->old_path, old_cache);
      rename(old_cache, cache);
      index_entry_t ne = {se->path_hash, se->content_hash, se->size, se->mtime, se->type, 0, xstrdup(se->path)};
      if (! ne.path) goto done;
      if (old_j >= 0) {
        free(idx.v[old_j].path);
        memmove(idx.v + old_j, idx.v + old_j + 1, (idx.n - old_j - 1) * sizeof(index_entry_t));
        idx.n--;
        if (j > old_j) j--;
      }
      if (j >= 0) { free(idx.v[j].path); idx.v[j] = ne; }
      else index_push(&idx, ne);
    } else if (se->action != STAGE_DEL && read_bytes(spath, &b)) {
      write_bytes(cache, b.data, b.n);
      index_entry_t ne = {se->path_hash, se->content_hash, se->size, se->mtime, se->type, 0, xstrdup(se->path)};
      if (j >= 0) { free(idx.v[j].path); idx.v[j] = ne; }
      else index_push(&idx, ne);
    } else if (j >= 0) {
      unlink(cache);
      free(idx.v[j].path);
      memmove(idx.v + j, idx.v + j + 1, (idx.n - j - 1) * sizeof(index_entry_t));
      idx.n--;
    }
    free_bytes(&b);
  }
  save_index(repo, &idx);
  clear_stage(repo, &stage);
  print_action_msg("amend", head, c.time, msg ? msg : c.msg);
  rc = 0;
done:
  paths_free(&paths);
  commit_free(&c);
  stage_free(&stage);
  index_free(&idx);
  return rc;
}

int command_squash(repo_t * repo, int argc, char ** argv) {
  if (argc != 1) { fprintf(stderr, "usage: sc squash COMMIT|HEAD|START..END|ALL\n"); return 1; }
  ids_t chain = {0}, ids = {0};
  paths_t paths = {0};
  index_t idx = {0};
  unsigned long long start = 0, end = 0, keep = 0, parent = 0;
  int lone = 0, rc = 1;
  char * msg = NULL;
  if (argc == 1 && strcmp(argv[0], "ALL") == 0) {
    if (! read_chain(repo, &chain)) goto done;
    if (chain.n <= 1) { printf("nothing to squash\n"); rc = 0; goto done; }
    if (! repo_clean(repo, "squash")) goto done;
    commit_t last = {0};
    if (! read_commit(repo, chain.v[0], &last)) { commit_free(&last); goto done; }
    long long t = last.time;
    commit_free(&last);
    if (! range_paths(repo, &chain, &paths) || ! build_squash_message(repo, &chain, &msg)) goto done;
    if (! load_index(repo, &idx) || ! write_squashed_commit(repo, &idx, 0, chain.v[0], 0, t, msg, &paths, 1)) goto done;
    for (int i = 0; i < chain.n; i++) if (chain.v[i] != 0) unlink_commit(repo, chain.v[i]);
    write_head_id(repo, 0);
    printf("squash ALL -> 0\n");
    rc = 0;
    goto done;
  }
  if (! read_chain(repo, &chain) || ! resolve_squash(repo, &chain, argv[0], &start, &end, &keep, &lone)) {
    fprintf(stderr, "invalid squash range\n");
    goto done;
  }
  if (! repo_clean(repo, "squash")) goto done;
  if (lone) { rc = squash_lone(repo); goto done; }
  commit_t first = {0}, last = {0};
  if (! read_commit(repo, start, &first) || ! read_commit(repo, end, &last)) { commit_free(&first); commit_free(&last); goto done; }
  parent = first.parent;
  long long t = last.time;
  commit_free(&first);
  commit_free(&last);
  if (! range_ids(repo, start, end, &ids) || ! range_paths(repo, &ids, &paths) || ! build_squash_message(repo, &ids, &msg)) goto done;
  if (! load_index(repo, &idx) || ! write_squashed_commit(repo, &idx, keep, end, parent, t, msg, &paths, start == 0)) goto done;
  for (int i = 0; i < ids.n; i++) if (ids.v[i] != keep) unlink_commit(repo, ids.v[i]);
  if (keep != end) (keep == 0 ? write_head_id(repo, 0) : write_head(repo, keep));
  printf("squash %llu..%llu -> %llu\n", start, end, keep);
  rc = 0;
done:
  free(msg);
  ids_free(&chain);
  ids_free(&ids);
  paths_free(&paths);
  index_free(&idx);
  return rc;
}

void print_log_msg(unsigned long long id, const char * ts, char * msg) {
  if (! strchr(msg, '\n')) {
    printf("%llu %s %s\n", id, ts, msg);
    return;
  }
  printf("%llu %s\n", id, ts);
  for (char * p = msg; *p;) {
    char * e = strchr(p, '\n');
    size_t n = e ? (size_t) (e - p) : strlen(p);
    if (n) { printf("  "); fwrite(p, 1, n, stdout); printf("\n"); }
    if (! e) break;
    p = e + 1;
  }
}

void trace_hunks_free(trace_hunks_t * hs) {
  free(hs->v);
  hs->v = NULL;
  hs->n = hs->size = 0;
}

int trace_hunks_push(trace_hunks_t * hs, int old_start, int old_n, int new_start, int new_n) {
  if (! old_n && ! new_n) return 1;
  if (hs->n == hs->size) {
    hs->size = hs->size ? 2 * hs->size : INITIAL_SIZE;
    trace_hunk_t * v = realloc(hs->v, hs->size * sizeof(trace_hunk_t));
    if (! v) return 0;
    hs->v = v;
  }
  hs->v[hs->n++] = (trace_hunk_t){old_start, old_n, new_start, new_n};
  return 1;
}

int build_trace_hunks(line_t * a, int an, line_t * b, int bn, trace_hunks_t * hs) {
  long long cells = (long long) (an + 1) * (long long) (bn + 1);
  if (cells > DIFF_CELL_LIMIT) return trace_hunks_push(hs, 0, an, 0, bn);
  int cols = bn + 1;
  int * dp = calloc((an + 1) * (bn + 1), sizeof(int));
  if (! dp) return 0;
  for (int i = an - 1; i >= 0; i--)
    for (int j = bn - 1; j >= 0; j--)
      dp[i * cols + j] = line_eq(&a[i], &b[j]) ? 1 + dp[(i + 1) * cols + j + 1] :
        (dp[(i + 1) * cols + j] > dp[i * cols + j + 1] ? dp[(i + 1) * cols + j] : dp[i * cols + j + 1]);
  int ok = 1;
  for (int i = 0, j = 0; ok && (i < an || j < bn);) {
    if (i < an && j < bn && line_eq(&a[i], &b[j])) { i++; j++; continue; }
    int oi = i, nj = j;
    while (i < an || j < bn) {
      if (i < an && j < bn && line_eq(&a[i], &b[j])) break;
      if (j >= bn || (i < an && dp[(i + 1) * cols + j] >= dp[i * cols + j + 1])) i++;
      else j++;
    }
    ok = trace_hunks_push(hs, oi, i - oi, nj, j - nj);
  }
  free(dp);
  return ok;
}

int parse_line_range(const char * s, int * from, int * to) {
  char * end = NULL;
  long a = strtol(s, &end, 10), b = a;
  if (a < 1) return 0;
  if (*end == ':') {
    char * bend = NULL;
    b = strtol(end + 1, &bend, 10);
    if (*bend || b < a) return 0;
  } else if (*end) return 0;
  *from = (int) a;
  *to = (int) b;
  return 1;
}

int parse_trace_range(repo_t * repo, const char * arg, unsigned long long * start, unsigned long long * end) {
  char buf[256];
  char * dots = strstr(arg, "..");
  if (! dots) return 0;
  size_t n = (size_t) (dots - arg);
  if (! n || n >= sizeof(buf)) return 0;
  memcpy(buf, arg, n);
  buf[n] = '\0';
  return parse_commit_ref(repo, buf, start) && parse_commit_ref(repo, dots + 2, end);
}

int resolve_trace_range(repo_t * repo, ids_t * chain, const char * range, int after_set, unsigned long long after, int before_set, unsigned long long before, int * first, int * last) {
  if (! chain->n) return 0;
  if (range) {
    unsigned long long start = 0, end = 0;
    if (after_set || before_set || ! parse_trace_range(repo, range, &start, &end)) return 0;
    int si = ids_find(chain, start), ei = ids_find(chain, end);
    if (si < 0 || ei < 0 || si < ei) return 0;
    *first = ei;
    *last = si;
    return 1;
  }
  *first = before_set ? ids_find(chain, before) : 0;
  if (*first < 0) return 0;
  if (after_set) {
    int ai = ids_find(chain, after);
    if (ai < 0) return 0;
    *last = ai - 1;
  } else {
    *last = chain->n - 1;
  }
  return 1;
}

void trace_commit_header(commit_t * c) {
  time_t tt = (time_t) c->time;
  char * ts = ctime(&tt);
  if (ts) ts[strcspn(ts, "\n")] = '\0';
  print_log_msg(c->id, ts ? ts : "unknown time", c->msg ? c->msg : "");
}

int span_intersects(int a1, int a2, int b1, int b2) {
  return a1 <= b2 && b1 <= a2;
}

int trace_line_match(trace_hunk_t * h, int from, int to) {
  if (! h->new_n) return 0;
  return span_intersects(from, to, h->new_start + 1, h->new_start + h->new_n);
}

int trace_regex_line(const char * regex, line_t * l) {
  char * s = malloc(l->n + 1);
  int start = -1, end = 0;
  if (! s) return 0;
  memcpy(s, l->s, l->n);
  s[l->n] = '\0';
  match(regex, s, &start, &end);
  free(s);
  return start >= 0;
}

int trace_regex_match(const char * regex, trace_hunk_t * h, line_t * old, line_t * new) {
  for (int i = 0; i < h->old_n; i++)
    if (trace_regex_line(regex, &old[h->old_start + i])) return 1;
  for (int i = 0; i < h->new_n; i++)
    if (trace_regex_line(regex, &new[h->new_start + i])) return 1;
  return 0;
}

void trace_add_span(int * from, int * to, int a, int b) {
  if (a > b) return;
  if (! *from || a < *from) *from = a;
  if (b > *to) *to = b;
}

int trace_map_lines(trace_hunks_t * hs, int new_total, int * from, int * to) {
  int next_new = 1, next_old = 1, pf = 0, pt = 0;
  for (int i = 0; i < hs->n; i++) {
    trace_hunk_t * h = &hs->v[i];
    int ns = h->new_start + 1, ne = h->new_start + h->new_n;
    int os = h->old_start + 1, oe = h->old_start + h->old_n;
    int uf = *from > next_new ? *from : next_new;
    int ut = *to < ns - 1 ? *to : ns - 1;
    if (uf <= ut) trace_add_span(&pf, &pt, next_old + (uf - next_new), next_old + (ut - next_new));
    if (h->new_n && h->old_n && span_intersects(*from, *to, ns, ne)) trace_add_span(&pf, &pt, os, oe);
    next_new = ne + 1;
    next_old = oe + 1;
  }
  int uf = *from > next_new ? *from : next_new;
  if (uf <= *to && uf <= new_total) trace_add_span(&pf, &pt, next_old + (uf - next_new), next_old + ((*to < new_total ? *to : new_total) - next_new));
  *from = pf;
  *to = pt;
  return pf != 0;
}

void print_trace_hunk(const char * path, trace_hunk_t * h, line_t * old, int old_n, line_t * new, int new_n, int * printed_path) {
  int color = isatty(STDOUT_FILENO);
  const char * red = color ? "\033[31m" : "";
  const char * green = color ? "\033[32m" : "";
  const char * cyan = color ? "\033[36m" : "";
  const char * dim = color ? "\033[2m" : "";
  const char * reset = color ? "\033[0m" : "";
  if (! *printed_path) {
    printf("%s------------------------------------------------------------%s\n", dim, reset);
    printf("%s%s (trace)%s\n", cyan, path, reset);
    *printed_path = 1;
  }
  int before = h->new_start - DIFF_CONTEXT_LINES;
  if (before < 0) before = 0;
  for (int i = before; i < h->new_start && i < new_n; i++) print_diff_line(dim, ' ', &new[i], reset);
  for (int i = 0; i < h->old_n && h->old_start + i < old_n; i++) print_diff_line(red, '-', &old[h->old_start + i], reset);
  for (int i = 0; i < h->new_n && h->new_start + i < new_n; i++) print_diff_line(green, '+', &new[h->new_start + i], reset);
  for (int i = h->new_start + h->new_n, end = i + DIFF_CONTEXT_LINES; i < end && i < new_n; i++) print_diff_line(dim, ' ', &new[i], reset);
}

int command_trace(repo_t * repo, int argc, char ** argv) {
  const char * regex = NULL, * range = NULL;
  char filter_buf[PATH_MAX], * filter = NULL;
  int line_from = 0, line_to = 0, after_set = 0, before_set = 0;
  unsigned long long after = 0, before = 0;
  if (argc < 1 || argv[0][0] == '-') { fprintf(stderr, "usage: sc trace <path> [--lines A[:B]] [--regex REGEX] [START..END|--after COMMIT|--before COMMIT]\n"); return 1; }
  if (rel_path(repo, argv[0], filter_buf)) filter = filter_buf;
  else { snprintf(filter_buf, sizeof(filter_buf), "%s", argv[0]); filter = filter_buf; }
  normalize_filter_path(filter);
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--lines") == 0) {
      if (i + 1 >= argc || ! parse_line_range(argv[++i], &line_from, &line_to)) { fprintf(stderr, "invalid line range\n"); return 1; }
    } else if (strcmp(argv[i], "--regex") == 0) {
      if (i + 1 >= argc) { fprintf(stderr, "usage: sc trace <path> [--lines A[:B]] [--regex REGEX] [START..END|--after COMMIT|--before COMMIT]\n"); return 1; }
      regex = argv[++i];
    } else if (strcmp(argv[i], "--after") == 0) {
      if (i + 1 >= argc || ! parse_commit_ref(repo, argv[++i], &after)) { fprintf(stderr, "unknown commit: %s\n", i < argc ? argv[i] : ""); return 1; }
      after_set = 1;
    } else if (strcmp(argv[i], "--before") == 0) {
      if (i + 1 >= argc || ! parse_commit_ref(repo, argv[++i], &before)) { fprintf(stderr, "unknown commit: %s\n", i < argc ? argv[i] : ""); return 1; }
      before_set = 1;
    } else if (! range && strstr(argv[i], "..")) {
      range = argv[i];
    } else {
      fprintf(stderr, "usage: sc trace <path> [--lines A[:B]] [--regex REGEX] [START..END|--after COMMIT|--before COMMIT]\n");
      return 1;
    }
  }

  ids_t chain = {0};
  index_t idx = {0};
  int first = 0, last = -1, rc = 1;
  if (! read_chain(repo, &chain)) goto done;
  if (! resolve_trace_range(repo, &chain, range, after_set, after, before_set, before, &first, &last)) { fprintf(stderr, "invalid trace range\n"); goto done; }
  if (! load_index(repo, &idx)) goto done;

  if (line_from && first <= last) {
    bytes_t anchor = {0};
    int type = SOURCE_NONE, exists = 0;
    if (! state_at_commit(repo, &idx, filter, chain.v[first], &anchor, &type, &exists) || ! exists || type != SOURCE_FILE || binary_like(&anchor)) {
      fprintf(stderr, "--lines requires a text file at the trace endpoint\n");
      free_bytes(&anchor);
      goto done;
    }
    line_t * lines = NULL;
    int n = split_lines(&anchor, &lines);
    free(lines);
    if (line_to > n) { fprintf(stderr, "line range outside file\n"); free_bytes(&anchor); goto done; }
    free_bytes(&anchor);
  }

  rc = 0;
  int trace_alive = 1;
  for (int p = first; p <= last && trace_alive; p++) {
    commit_t c = {0};
    if (! read_commit(repo, chain.v[p], &c)) { rc = 1; break; }
    commit_file_t * f = commit_file(&c, filter);
    if (! f) { commit_free(&c); continue; }
    bytes_t old = {0}, new = {0};
    int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0;
    int root_old = c.id == 0 || (c.parent == 0 && ! commit_exists(repo, 0));
    if (root_old) empty_bytes(&old);
    else if (! state_at_commit(repo, &idx, filter, c.parent, &old, &old_type, &old_exists)) { rc = 1; commit_free(&c); break; }
    if (! state_at_commit(repo, &idx, filter, c.id, &new, &new_type, &new_exists)) { free_bytes(&old); rc = 1; commit_free(&c); break; }

    if (! line_from && ! regex) {
      int shown = 0;
      trace_commit_header(&c);
      print_one_diff(filter, &old, old_type, old_exists, &new, new_type, new_exists, &shown, 0);
    } else if ((! old_exists || old_type == SOURCE_FILE) && (! new_exists || new_type == SOURCE_FILE) && ! binary_like(&old) && ! binary_like(&new)) {
      line_t * a = NULL, * b = NULL;
      trace_hunks_t hs = {0};
      int an = split_lines(&old, &a), bn = split_lines(&new, &b), printed = 0, matched = 0;
      if (an < 0 || bn < 0 || ! build_trace_hunks(a, an, b, bn, &hs)) rc = 1;
      for (int i = 0; ! rc && i < hs.n; i++) {
        int ok = (! line_from || trace_line_match(&hs.v[i], line_from, line_to)) &&
          (! regex || trace_regex_match(regex, &hs.v[i], a, b));
        if (! ok) continue;
        if (! matched) { trace_commit_header(&c); matched = 1; }
        print_trace_hunk(filter, &hs.v[i], a, an, b, bn, &printed);
      }
      if (! rc && line_from && ! trace_map_lines(&hs, bn, &line_from, &line_to)) trace_alive = 0;
      trace_hunks_free(&hs);
      free(a);
      free(b);
    }
    free_bytes(&old);
    free_bytes(&new);
    commit_free(&c);
    if (rc) break;
  }
done:
  index_free(&idx);
  ids_free(&chain);
  return rc;
}

void format_bytes(unsigned long long n, char * out, size_t out_n) {
  const char * units[] = {"B", "KB", "MB", "GB"};
  double v = (double) n;
  int u = 0;
  while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
  if (u == 0) snprintf(out, out_n, "%lluB", n);
  else snprintf(out, out_n, "%.1f%s", v, units[u]);
}

unsigned long long commit_stored_size(repo_t * repo, unsigned long long id) {
  char path[PATH_MAX];
  struct stat st;
  snprintf(path, sizeof(path), "%s/commits/%016llx.sc", repo->sc, id);
  return stat(path, &st) == 0 ? (unsigned long long) st.st_size : 0;
}

const char * commit_file_action(commit_file_t * f) {
  if (f->kind == 1) return "added";
  if (f->kind == 2) return "deleted";
  if (f->kind == 3) return "moved";
  if (f->old_type != f->new_type) return "type-change";
  if (f->new_type == SOURCE_LINK) return "link-change";
  return "modified";
}

void first_msg_line(char * msg, char * out, size_t out_n) {
  size_t n = strcspn(msg ? msg : "", "\n");
  if (n >= out_n) n = out_n - 1;
  memcpy(out, msg ? msg : "", n);
  out[n] = '\0';
}

int cmp_commit_files_stored(const void * a, const void * b) {
  const commit_file_t * x = (const commit_file_t*) a;
  const commit_file_t * y = (const commit_file_t*) b;
  if (x->stored_size != y->stored_size) return x->stored_size < y->stored_size ? 1 : -1;
  return strcmp(x->path, y->path);
}

void print_browse_commit_list(repo_t * repo) {
  unsigned long long head = 0;
  if (! read_head_id(repo, &head)) return;
  printf("commit   stored   files  message\n");
  while (1) {
    commit_t c = {0};
    if (! read_commit(repo, head, &c)) return;
    char size[32], msg[160];
    format_bytes(commit_stored_size(repo, head), size, sizeof(size));
    first_msg_line(c.msg, msg, sizeof(msg));
    printf("%-8llu %-8s %-5d %s\n", head, size, c.n, msg);
    if (head == 0 || (c.parent == 0 && ! commit_exists(repo, 0))) { commit_free(&c); break; }
    head = c.parent;
    commit_free(&c);
  }
}

unsigned long long edit_stored_size(edit_t * e) {
  char header[128];
  int n = snprintf(header, sizeof(header), "edit %zu %zu %zu\n", e->off, e->new_n, e->old_n);
  return (unsigned long long) (n > 0 ? n : 0) + e->old_n + 1;
}

int line_diff_counts(line_t * a, int an, line_t * b, int bn, int * added, int * deleted) {
  *added = *deleted = 0;
  int pre = 0, suf = 0;
  while (pre < an && pre < bn && line_eq(&a[pre], &b[pre])) pre++;
  while (suf + pre < an && suf + pre < bn && line_eq(&a[an - 1 - suf], &b[bn - 1 - suf])) suf++;
  an -= pre + suf;
  bn -= pre + suf;
  if ((long long) (an + 1) * (long long) (bn + 1) > STATUS_CELL_LIMIT) return 0;
  int cols = bn + 1;
  int * dp = calloc((size_t) (an + 1) * (size_t) (bn + 1), sizeof(int));
  if (! dp) return 0;
  for (int i = an - 1; i >= 0; i--)
    for (int j = bn - 1; j >= 0; j--)
      dp[i * cols + j] = line_eq(&a[pre + i], &b[pre + j]) ? 1 + dp[(i + 1) * cols + j + 1] :
        (dp[(i + 1) * cols + j] > dp[i * cols + j + 1] ? dp[(i + 1) * cols + j] : dp[i * cols + j + 1]);
  *deleted = an - dp[0];
  *added = bn - dp[0];
  free(dp);
  return 1;
}

void edit_line_range(bytes_t * old, int old_exists, line_t * a, int an, bytes_t * new, int new_exists, line_t * b, int bn, edit_t * e, int * ai, int * ac, int * bi, int * bc) {
  *ai = old_exists ? line_at_offset(a, an, old->data, e->off < old->n ? e->off : old->n) : 0;
  *bi = new_exists ? line_at_offset(b, bn, new->data, e->off < new->n ? e->off : new->n) : 0;
  *ac = e->old_n ? line_at_offset(a, an, old->data, e->off + e->old_n - 1 < old->n ? e->off + e->old_n - 1 : old->n) - *ai + 1 : (old_exists && an ? 1 : 0);
  *bc = e->new_n ? line_at_offset(b, bn, new->data, e->off + e->new_n - 1 < new->n ? e->off + e->new_n - 1 : new->n) - *bi + 1 : (new_exists && bn ? 1 : 0);
}

void edit_line_window(bytes_t * old, int old_exists, line_t * a, int an, bytes_t * new, int new_exists, line_t * b, int bn, edit_t * e, int * ai, int * ac, int * bi, int * bc) {
  edit_line_range(old, old_exists, a, an, new, new_exists, b, bn, e, ai, ac, bi, bc);
  int margin = DIFF_CONTEXT_LINES * 80;
  int old_hi = *ai + *ac + margin;
  int new_hi = *bi + *bc + margin;
  *ai = *ai < margin ? 0 : *ai - margin;
  *bi = *bi < margin ? 0 : *bi - margin;
  if (old_hi > an) old_hi = an;
  if (new_hi > bn) new_hi = bn;
  *ac = old_hi - *ai;
  *bc = new_hi - *bi;
}

int edit_line_counts(bytes_t * old, int old_exists, line_t * a, int an, bytes_t * new, int new_exists, line_t * b, int bn, edit_t * e, int * added, int * deleted) {
  int ai = 0, ac = 0, bi = 0, bc = 0;
  trace_hunks_t hs = {0};
  if (an < 0 || bn < 0) return 0;
  edit_line_range(old, old_exists, a, an, new, new_exists, b, bn, e, &ai, &ac, &bi, &bc);
  int margin = DIFF_CONTEXT_LINES * 80;
  int ao = ai < margin ? 0 : ai - margin;
  int bo = bi < margin ? 0 : bi - margin;
  int ah = ai + ac + margin;
  int bh = bi + bc + margin;
  if (ah > an) ah = an;
  if (bh > bn) bh = bn;
  if (! build_trace_hunks(a + ao, ah - ao, b + bo, bh - bo, &hs)) {
    if (! ac || ! bc) {
      *added = bc;
      *deleted = ac;
      return 1;
    }
    return line_diff_counts(a + ai, ac, b + bi, bc, added, deleted);
  }
  *added = *deleted = 0;
  for (int i = 0; i < hs.n; i++) {
    trace_hunk_t * h = &hs.v[i];
    int hs_old = ao + h->old_start, hs_new = bo + h->new_start;
    int old_hit = ac && h->old_n && hs_old < ai + ac && hs_old + h->old_n > ai;
    int new_hit = bc && h->new_n && hs_new < bi + bc && hs_new + h->new_n > bi;
    if (old_hit || new_hit) {
      *deleted += h->old_n;
      *added += h->new_n;
    }
  }
  trace_hunks_free(&hs);
  return 1;
}

void print_browse_file(repo_t * repo, unsigned long long id, commit_file_t * f) {
  char stored[32], old_size[32], new_size[32];
  bytes_t old = {0}, new = {0};
  line_t * a = NULL, * b = NULL;
  int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0, an = -1, bn = -1, text = 0;
  format_bytes(f->stored_size, stored, sizeof(stored));
  format_bytes(f->old_size, old_size, sizeof(old_size));
  format_bytes(f->new_size, new_size, sizeof(new_size));
  if (focused_diff_states(repo, id, f, &old, &old_type, &old_exists, &new, &new_type, &new_exists) &&
      old_type != SOURCE_LINK && new_type != SOURCE_LINK && (! old_exists || ! binary_like(&old)) && (! new_exists || ! binary_like(&new))) {
    an = split_lines(&old, &a);
    bn = split_lines(&new, &b);
    text = an >= 0 && bn >= 0;
  }
  if (f->old_path) printf("path: %s -> %s\n", f->old_path, f->path);
  else printf("path: %s\n", f->path);
  printf("action: %s\n", commit_file_action(f));
  printf("stored: %s (%llu bytes)\n", stored, f->stored_size);
  printf("content: %s -> %s\n", old_size, new_size);
  printf("edits: %d\n", f->edits.n);
  for (int i = 0; i < f->edits.n; i++) {
    char size[32], added[16] = "?", deleted[16] = "?";
    int add = 0, del = 0;
    format_bytes(edit_stored_size(&f->edits.v[i]), size, sizeof(size));
    if (text && edit_line_counts(&old, old_exists, a, an, &new, new_exists, b, bn, &f->edits.v[i], &add, &del)) {
      snprintf(added, sizeof(added), "%d", add);
      snprintf(deleted, sizeof(deleted), "%d", del);
    }
    printf("  edit %-3d offset %-8zu size %-8s additions %-4s deletions %-4s\n",
      i + 1, f->edits.v[i].off, size, added, deleted);
  }
  free(a);
  free(b);
  free_bytes(&old);
  free_bytes(&new);
}

void print_browse_commit(repo_t * repo, unsigned long long id) {
  commit_t c = {0};
  if (! read_commit(repo, id, &c)) { fprintf(stderr, "unknown commit: %llu\n", id); return; }
  qsort(c.files, c.n, sizeof(commit_file_t), cmp_commit_files_stored);
  char total[32], msg[160];
  format_bytes(commit_stored_size(repo, id), total, sizeof(total));
  first_msg_line(c.msg, msg, sizeof(msg));
  printf("commit %llu  stored %s  files %d\n%s\n", id, total, c.n, msg);
  printf("\nstored   action       content        path\n");
  for (int i = 0; i < c.n; i++) {
    commit_file_t * f = &c.files[i];
    char stored[32], old_size[32], new_size[32];
    format_bytes(f->stored_size, stored, sizeof(stored));
    format_bytes(f->old_size, old_size, sizeof(old_size));
    format_bytes(f->new_size, new_size, sizeof(new_size));
    printf("%-8s %-12s %s -> %-8s %s", stored, commit_file_action(f), old_size, new_size, f->path);
    if (f->old_path) printf("  (from %s)", f->old_path);
    printf("\n");
  }
  commit_free(&c);
}

int browse_resolve_file(repo_t * repo, const char * arg, char * out) {
  if (rel_path(repo, arg, out)) normalize_filter_path(out);
  else { snprintf(out, PATH_MAX, "%s", arg); normalize_filter_path(out); }
  return 1;
}

int browse_noninteractive(repo_t * repo, int argc, char ** argv) {
  if (argc == 0) { print_browse_commit_list(repo); return 0; }
  if (argc > 2) { fprintf(stderr, "usage: sc browse [COMMIT] [FILE]\n"); return 1; }
  unsigned long long id = 0;
  if (! parse_commit_ref(repo, argv[0], &id)) { fprintf(stderr, "unknown commit: %s\n", argv[0]); return 1; }
  if (argc == 1) { print_browse_commit(repo, id); return 0; }
  commit_t c = {0};
  char path[PATH_MAX];
  browse_resolve_file(repo, argv[1], path);
  if (! read_commit(repo, id, &c)) { fprintf(stderr, "unknown commit: %llu\n", id); return 1; }
  commit_file_t * f = commit_file(&c, path);
  if (! f) { fprintf(stderr, "commit %llu did not touch: %s\n", id, path); commit_free(&c); return 1; }
  printf("commit %llu\n", id);
  print_browse_file(repo, id, f);
  commit_free(&c);
  return 0;
}

void browse_clear(void) {
  printf("\033[2J\033[H");
}

void browse_screen_enter(void) {
  if (isatty(STDOUT_FILENO)) printf("\033[?1049h\033[?25l");
}

void browse_screen_leave(void) {
  if (isatty(STDOUT_FILENO)) printf("\033[?25h\033[?1049l");
}

void browse_size(int * rows, int * cols) {
  struct winsize ws;
  *rows = 24;
  *cols = 80;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_row) *rows = ws.ws_row;
    if (ws.ws_col) *cols = ws.ws_col;
  }
}

int browse_file_edit_rows(unsigned long long id) {
  int rows, cols;
  char header[160];
  browse_size(&rows, &cols);
  snprintf(header, sizeof(header), "sc browse: commit %llu file  enter/right=open edit  left/backspace=back  q=quit", id);
  int max = rows - browse_text_rows(header, cols) - 7;
  if (max > 18) return 18;
  return max > 0 ? max : 1;
}

int browse_file_edit_start_row(unsigned long long id) {
  int rows, cols;
  char header[160];
  browse_size(&rows, &cols);
  (void) rows;
  snprintf(header, sizeof(header), "sc browse: commit %llu file  enter/right=open edit  left/backspace=back  q=quit", id);
  return browse_text_rows(header, cols) + 7;
}

const char * browse_color(const char * code) {
  return isatty(STDOUT_FILENO) ? code : "";
}

void browse_row(int selected) {
  if (selected) printf("%s>%s ", browse_color("\033[7m"), browse_color("\033[0m"));
  else printf("  ");
}

void browse_select_row(int row, int selected) {
  printf("\033[%d;1H", row);
  browse_row(selected);
}

void browse_move_selection(int old_row, int new_row) {
  browse_select_row(old_row, 0);
  browse_select_row(new_row, 1);
  fflush(stdout);
}

int browse_selected_file(repo_t * repo, unsigned long long id, int selected, commit_t * c, commit_file_t ** f) {
  if (! read_commit(repo, id, c)) return 0;
  qsort(c->files, c->n, sizeof(commit_file_t), cmp_commit_files_stored);
  if (selected < 0 || selected >= c->n) return 0;
  *f = &c->files[selected];
  return 1;
}

void browse_render_commits(repo_t * repo, ids_t * chain, int selected, int offset) {
  const char * dim = browse_color("\033[2m");
  const char * cyan = browse_color("\033[36m");
  const char * yellow = browse_color("\033[33m");
  const char * reset = browse_color("\033[0m");
  browse_clear();
  printf("%ssc browse: commits  enter/right=open  q=quit%s\n\n", dim, reset);
  for (int i = offset; i < chain->n && i < offset + 20; i++) {
    commit_t c = {0};
    if (! read_commit(repo, chain->v[i], &c)) continue;
    char size[32], msg[120];
    format_bytes(commit_stored_size(repo, chain->v[i]), size, sizeof(size));
    first_msg_line(c.msg, msg, sizeof(msg));
    browse_row(i == selected);
    printf("%s%-6llu%s %s%-8s%s %3d  %s\n", cyan, chain->v[i], reset, yellow, size, reset, c.n, msg);
    commit_free(&c);
  }
  fflush(stdout);
}

void browse_render_files(repo_t * repo, unsigned long long id, int selected, int offset) {
  commit_t c = {0};
  const char * dim = browse_color("\033[2m");
  const char * cyan = browse_color("\033[36m");
  const char * yellow = browse_color("\033[33m");
  const char * green = browse_color("\033[32m");
  const char * red = browse_color("\033[31m");
  const char * reset = browse_color("\033[0m");
  browse_clear();
  if (! read_commit(repo, id, &c)) { printf("unknown commit: %llu\n", id); fflush(stdout); return; }
  qsort(c.files, c.n, sizeof(commit_file_t), cmp_commit_files_stored);
  printf("%ssc browse: commit %llu  enter/right=open  left/backspace=back  q=quit%s\n\n", dim, id, reset);
  for (int i = offset; i < c.n && i < offset + 20; i++) {
    commit_file_t * f = &c.files[i];
    char stored[32], old_size[32], new_size[32];
    format_bytes(f->stored_size, stored, sizeof(stored));
    format_bytes(f->old_size, old_size, sizeof(old_size));
    format_bytes(f->new_size, new_size, sizeof(new_size));
    browse_row(i == selected);
    printf("%s%-8s%s %-12s %s%s%s -> %s%-8s%s %s%s%s\n",
      yellow, stored, reset, commit_file_action(f), red, old_size, reset, green, new_size, reset, cyan, f->path, reset);
  }
  fflush(stdout);
  commit_free(&c);
}

void browse_print_file_edit(commit_file_t * f, int i, int edit_i, int text, bytes_t * old, int old_exists, line_t * a, int an, bytes_t * new, int new_exists, line_t * b, int bn, const char * yellow, const char * green, const char * red, const char * reset) {
  char size[32], added[16] = "?", deleted[16] = "?";
  int add = 0, del = 0;
  format_bytes(edit_stored_size(&f->edits.v[i]), size, sizeof(size));
  if (text && edit_line_counts(old, old_exists, a, an, new, new_exists, b, bn, &f->edits.v[i], &add, &del)) {
    snprintf(added, sizeof(added), "%d", add);
    snprintf(deleted, sizeof(deleted), "%d", del);
  }
  browse_row(i == edit_i);
  printf("edit %-3d offset %-8zu size %s%-8s%s additions %s%-4s%s deletions %s%-4s%s\n",
    i + 1, f->edits.v[i].off, yellow, size, reset, green, added, reset, red, deleted, reset);
}

void browse_render_file(repo_t * repo, unsigned long long id, int selected, int edit_i, int edit_off) {
  commit_t c = {0};
  commit_file_t * f = NULL;
  bytes_t old = {0}, new = {0};
  line_t * a = NULL, * b = NULL;
  int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0, an = -1, bn = -1, text = 0;
  const char * dim = browse_color("\033[2m");
  const char * cyan = browse_color("\033[36m");
  const char * yellow = browse_color("\033[33m");
  const char * green = browse_color("\033[32m");
  const char * red = browse_color("\033[31m");
  const char * reset = browse_color("\033[0m");
  browse_clear();
  if (! browse_selected_file(repo, id, selected, &c, &f)) { printf("unknown commit/file\n"); fflush(stdout); return; }
  char stored[32], old_size[32], new_size[32];
  format_bytes(f->stored_size, stored, sizeof(stored));
  format_bytes(f->old_size, old_size, sizeof(old_size));
  format_bytes(f->new_size, new_size, sizeof(new_size));
  printf("%ssc browse: commit %llu file  enter/right=open edit  left/backspace=back  q=quit%s\n\n", dim, id, reset);
  printf("path: %s%s%s\n", cyan, f->path, reset);
  printf("action: %s\n", commit_file_action(f));
  printf("stored: %s%s%s (%llu bytes)\n", yellow, stored, reset, f->stored_size);
  printf("content: %s%s%s -> %s%s%s\n", red, old_size, reset, green, new_size, reset);
  printf("edits: %d\n", f->edits.n);
  if (focused_diff_states(repo, id, f, &old, &old_type, &old_exists, &new, &new_type, &new_exists) &&
      old_type != SOURCE_LINK && new_type != SOURCE_LINK && (! old_exists || ! binary_like(&old)) && (! new_exists || ! binary_like(&new))) {
    an = split_lines(&old, &a);
    bn = split_lines(&new, &b);
    text = an >= 0 && bn >= 0;
  }
  int edit_rows = browse_file_edit_rows(id);
  for (int i = edit_off; i < f->edits.n && i < edit_off + edit_rows; i++) {
    browse_print_file_edit(f, i, edit_i, text, &old, old_exists, a, an, &new, new_exists, b, bn, yellow, green, red, reset);
  }
  if (! f->edits.n) printf("%sno byte edits stored for this record%s\n", dim, reset);
  free(a);
  free(b);
  free_bytes(&old);
  free_bytes(&new);
  fflush(stdout);
  commit_free(&c);
}

void browse_render_file_edits(repo_t * repo, unsigned long long id, int selected, int edit_i, int edit_off) {
  commit_t c = {0};
  commit_file_t * f = NULL;
  bytes_t old = {0}, new = {0};
  line_t * a = NULL, * b = NULL;
  int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0, an = -1, bn = -1, text = 0;
  const char * yellow = browse_color("\033[33m");
  const char * green = browse_color("\033[32m");
  const char * red = browse_color("\033[31m");
  const char * reset = browse_color("\033[0m");
  if (! browse_selected_file(repo, id, selected, &c, &f)) return;
  if (focused_diff_states(repo, id, f, &old, &old_type, &old_exists, &new, &new_type, &new_exists) &&
      old_type != SOURCE_LINK && new_type != SOURCE_LINK && (! old_exists || ! binary_like(&old)) && (! new_exists || ! binary_like(&new))) {
    an = split_lines(&old, &a);
    bn = split_lines(&new, &b);
    text = an >= 0 && bn >= 0;
  }
  int start = browse_file_edit_start_row(id), rows = browse_file_edit_rows(id);
  for (int row = 0; row < rows; row++) {
    int i = edit_off + row;
    printf("\033[%d;1H\033[2K", start + row);
    if (i < f->edits.n) browse_print_file_edit(f, i, edit_i, text, &old, old_exists, a, an, &new, new_exists, b, bn, yellow, green, red, reset);
    else printf("\n");
  }
  free(a);
  free(b);
  free_bytes(&old);
  free_bytes(&new);
  commit_free(&c);
  fflush(stdout);
}

int line_at_offset(line_t * lines, int n, const unsigned char * base, size_t off) {
  for (int i = 0; i < n; i++) {
    size_t start = (size_t) (lines[i].s - base);
    if (off < start + lines[i].n) return i;
  }
  return n ? n - 1 : 0;
}

int focused_diff_states(repo_t * repo, unsigned long long id, commit_file_t * f, bytes_t * old, int * old_type, int * old_exists, bytes_t * new, int * new_type, int * new_exists) {
  index_t idx = {0};
  commit_t c = {0};
  if (! load_index(repo, &idx) || ! read_commit(repo, id, &c)) { index_free(&idx); return 0; }
  unsigned long long parent = c.parent;
  int base_root = id == 0 || (parent == 0 && ! commit_exists(repo, 0));
  commit_free(&c);
  int ok = 1;
  if (base_root || f->kind == 1) {
    empty_bytes(old);
    *old_type = SOURCE_NONE;
    *old_exists = 0;
  } else ok = state_at_commit(repo, &idx, f->old_path ? f->old_path : f->path, parent, old, old_type, old_exists);
  if (ok) ok = state_at_commit(repo, &idx, f->path, id, new, new_type, new_exists);
  index_free(&idx);
  return ok;
}

int browse_edit_span(repo_t * repo, unsigned long long id, int selected, int edit_i, int * pre, int * ai, int * ac, int * bi, int * bc, int * post, int * binary) {
  commit_t c = {0};
  commit_file_t * f = NULL;
  *pre = *ai = *ac = *bi = *bc = *post = *binary = 0;
  if (! browse_selected_file(repo, id, selected, &c, &f) || edit_i < 0 || edit_i >= f->edits.n) { commit_free(&c); return 0; }
  bytes_t old = {0}, new = {0};
  int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0;
  if (! focused_diff_states(repo, id, f, &old, &old_type, &old_exists, &new, &new_type, &new_exists)) { commit_free(&c); return 0; }
  if (old_type == SOURCE_LINK || new_type == SOURCE_LINK || (old_exists && binary_like(&old)) || (new_exists && binary_like(&new))) {
    *binary = 1;
  } else {
    line_t * a = NULL, * b = NULL;
    int an = split_lines(&old, &a), bn = split_lines(&new, &b);
    edit_t * e = &f->edits.v[edit_i];
    *ai = old_exists ? line_at_offset(a, an, old.data, e->off < old.n ? e->off : old.n) : 0;
    *bi = new_exists ? line_at_offset(b, bn, new.data, e->off < new.n ? e->off : new.n) : 0;
    *ac = e->old_n ? line_at_offset(a, an, old.data, e->off + e->old_n - 1 < old.n ? e->off + e->old_n - 1 : old.n) - *ai + 1 : 0;
    *bc = e->new_n ? line_at_offset(b, bn, new.data, e->off + e->new_n - 1 < new.n ? e->off + e->new_n - 1 : new.n) - *bi + 1 : 0;
    int ctx = DIFF_CONTEXT_LINES;
    *pre = *bi < ctx ? *bi : ctx;
    int post_start = *bi + (*bc ? *bc : 1);
    *post = post_start < bn ? (bn - post_start < ctx ? bn - post_start : ctx) : 0;
    free(a);
    free(b);
  }
  free_bytes(&old);
  free_bytes(&new);
  commit_free(&c);
  return 1;
}

int browse_line_rows(line_t * line, int cols) {
  size_t n = line_chars(line);
  int body = cols > 4 ? cols - 3 : 1;
  return n ? (int) ((n + body - 1) / body) : 1;
}

int browse_text_rows(const char * text, int cols) {
  int body = cols > 1 ? cols - 1 : 1;
  size_t n = strlen(text);
  return n ? (int) ((n + body - 1) / body) : 1;
}

int browse_edit_body_rows(int rows, int cols, unsigned long long id, int edit_i) {
  char header[160];
  snprintf(header, sizeof(header), "sc browse: commit %llu file edit %d  up/down=scroll  left/backspace=back  q=quit", id, edit_i + 1);
  int max = rows - browse_text_rows(header, cols) - 3;
  return max > 0 ? max : 1;
}

void browse_emit_diff_line(int * row, int scroll, int max, int cols, const char * color, char prefix, line_t * line, const char * reset);

int browse_output_line(int * row, int scroll, int max, int cols, const char * color, char prefix, line_t * line, const char * reset, int emit) {
  int before = *row;
  if (emit) browse_emit_diff_line(row, scroll, max, cols, color, prefix, line, reset);
  else *row += browse_line_rows(line, cols);
  return *row - before;
}

int browse_output_text(int * row, int scroll, int max, int cols, const char * color, const char * text, const char * reset, int emit) {
  line_t line = {(unsigned char*) text, strlen(text)};
  return browse_output_line(row, scroll, max, cols, color, ' ', &line, reset, emit);
}

int browse_context_lines(int * row, line_t * lines, int start, int n, int scroll, int max, int cols, const char * dim, const char * reset, int emit) {
  int shown = 0;
  for (int i = 0; i < n; i++) shown += browse_output_line(row, scroll, max, cols, dim, ' ', &lines[start + i], reset, emit);
  return shown;
}

int browse_context_run(int * row, line_t * lines, int start, int n, int leading, int trailing, int scroll, int max, int cols, const char * dim, const char * reset, int emit) {
  int shown = 0;
  if (n <= 0) return 0;
  if (n <= 2 * DIFF_CONTEXT_LINES && ! leading && ! trailing)
    return browse_context_lines(row, lines, start, n, scroll, max, cols, dim, reset, emit);
  if (leading) {
    if (n > DIFF_CONTEXT_LINES) shown += browse_output_text(row, scroll, max, cols, dim, "...", reset, emit);
    shown += browse_context_lines(row, lines, start + (n > DIFF_CONTEXT_LINES ? n - DIFF_CONTEXT_LINES : 0), n > DIFF_CONTEXT_LINES ? DIFF_CONTEXT_LINES : n, scroll, max, cols, dim, reset, emit);
  } else if (trailing) {
    shown += browse_context_lines(row, lines, start, n > DIFF_CONTEXT_LINES ? DIFF_CONTEXT_LINES : n, scroll, max, cols, dim, reset, emit);
    if (n > DIFF_CONTEXT_LINES) shown += browse_output_text(row, scroll, max, cols, dim, "...", reset, emit);
  } else {
    shown += browse_context_lines(row, lines, start, DIFF_CONTEXT_LINES, scroll, max, cols, dim, reset, emit);
    shown += browse_output_text(row, scroll, max, cols, dim, "...", reset, emit);
    shown += browse_context_lines(row, lines, start + n - DIFF_CONTEXT_LINES, DIFF_CONTEXT_LINES, scroll, max, cols, dim, reset, emit);
  }
  return shown;
}

int browse_changed_rows(int * row, line_t * a, int an, line_t * b, int bn, int scroll, int max, int cols, const char * red, const char * green, const char * reset, int emit) {
  int shown = 0;
  for (int i = 0; i < an; i++) shown += browse_output_line(row, scroll, max, cols, red, '-', &a[i], reset, emit);
  for (int i = 0; i < bn; i++) shown += browse_output_line(row, scroll, max, cols, green, '+', &b[i], reset, emit);
  return shown;
}

int browse_lcs_rows(int * row, line_t * a, int an, line_t * b, int bn, int scroll, int max, int cols, const char * red, const char * green, const char * dim, const char * reset, int emit) {
  int pre = 0, suf = 0, shown = 0, changed = 0;
  if (! an || ! bn) return browse_changed_rows(row, a, an, b, bn, scroll, max, cols, red, green, reset, emit);
  while (pre < an && pre < bn && line_eq(&a[pre], &b[pre])) pre++;
  while (suf + pre < an && suf + pre < bn && line_eq(&a[an - 1 - suf], &b[bn - 1 - suf])) suf++;
  int ao = pre, bo = pre;
  an -= pre + suf;
  bn -= pre + suf;
  long long cells = (long long) (an + 1) * (long long) (bn + 1);
  if (cells > DIFF_CELL_LIMIT) return browse_output_text(row, scroll, max, cols, dim, "text diff omitted (comparison too large to summarize)", reset, emit);
  int * dp = calloc((size_t) (an + 1) * (size_t) (bn + 1), sizeof(int));
  if (! dp) return browse_changed_rows(row, a + ao, an, b + bo, bn, scroll, max, cols, red, green, reset, emit);
  int dp_cols = bn + 1;
  for (int i = an - 1; i >= 0; i--)
    for (int j = bn - 1; j >= 0; j--)
      dp[i * dp_cols + j] = line_eq(&a[ao + i], &b[bo + j]) ? 1 + dp[(i + 1) * dp_cols + j + 1] :
        (dp[(i + 1) * dp_cols + j] > dp[i * dp_cols + j + 1] ? dp[(i + 1) * dp_cols + j] : dp[i * dp_cols + j + 1]);
  shown += browse_context_run(row, a, 0, pre, 1, 0, scroll, max, cols, dim, reset, emit);
  for (int i = 0, j = 0; i < an || j < bn;) {
    if (i < an && j < bn && line_eq(&a[ao + i], &b[bo + j])) {
      int dj = j;
      while (i < an && j < bn && line_eq(&a[ao + i], &b[bo + j])) { i++; j++; }
      shown += browse_context_run(row, b, bo + dj, j - dj, ! changed, i == an && j == bn, scroll, max, cols, dim, reset, emit);
      continue;
    }
    int di = i, aj = j;
    while (i < an || j < bn) {
      if (i < an && j < bn && line_eq(&a[ao + i], &b[bo + j])) break;
      if (j >= bn || (i < an && dp[(i + 1) * dp_cols + j] >= dp[i * dp_cols + j + 1])) i++;
      else j++;
    }
    changed = 1;
    shown += browse_changed_rows(row, a + ao + di, i - di, b + bo + aj, j - aj, scroll, max, cols, red, green, reset, emit);
  }
  shown += browse_context_run(row, a, ao + an, suf, 0, 1, scroll, max, cols, dim, reset, emit);
  free(dp);
  return shown;
}

int browse_edit_diff_rows(bytes_t * old, int old_exists, line_t * a, int an, bytes_t * new, int new_exists, line_t * b, int bn, edit_t * e, int scroll, int max, int cols, const char * red, const char * green, const char * dim, const char * reset, int emit) {
  int ai = 0, ac = 0, bi = 0, bc = 0, row = 0, shown = 0, printed = 0, last_new = 0;
  edit_line_range(old, old_exists, a, an, new, new_exists, b, bn, e, &ai, &ac, &bi, &bc);
  int margin = DIFF_CONTEXT_LINES * 80;
  int ao = ai < margin ? 0 : ai - margin;
  int bo = bi < margin ? 0 : bi - margin;
  int ah = ai + ac + margin;
  int bh = bi + bc + margin;
  if (ah > an) ah = an;
  if (bh > bn) bh = bn;
  trace_hunks_t hs = {0};
  if (! build_trace_hunks(a + ao, ah - ao, b + bo, bh - bo, &hs))
    return browse_lcs_rows(&row, ac ? a + ai : NULL, ac, bc ? b + bi : NULL, bc, scroll, max, cols, red, green, dim, reset, emit);
  int * selected = calloc(hs.n ? hs.n : 1, sizeof(int));
  if (! selected) { trace_hunks_free(&hs); return 0; }
  for (int i = 0; i < hs.n; i++) {
    trace_hunk_t * h = &hs.v[i];
    int hs_old = ao + h->old_start, hs_new = bo + h->new_start;
    int old_hit = ac && h->old_n && hs_old < ai + ac && hs_old + h->old_n > ai;
    int new_hit = bc && h->new_n && hs_new < bi + bc && hs_new + h->new_n > bi;
    selected[i] = old_hit || new_hit;
  }
  for (int i = 0; i < hs.n; i++) {
    if (! selected[i]) continue;
    trace_hunk_t * h = &hs.v[i];
    int hs_old = ao + h->old_start, hs_new = bo + h->new_start;
    if (! printed) {
      int before = hs_new < DIFF_CONTEXT_LINES ? 0 : hs_new - DIFF_CONTEXT_LINES;
      shown += browse_context_lines(&row, b, before, hs_new - before, scroll, max, cols, dim, reset, emit);
    } else {
      int gap = hs_new - last_new;
      if (gap <= 2 * DIFF_CONTEXT_LINES) {
        shown += browse_context_lines(&row, b, last_new, gap, scroll, max, cols, dim, reset, emit);
      } else {
        shown += browse_context_lines(&row, b, last_new, DIFF_CONTEXT_LINES, scroll, max, cols, dim, reset, emit);
        shown += browse_output_text(&row, scroll, max, cols, dim, "...", reset, emit);
        shown += browse_context_lines(&row, b, hs_new - DIFF_CONTEXT_LINES, DIFF_CONTEXT_LINES, scroll, max, cols, dim, reset, emit);
      }
    }
    shown += browse_changed_rows(&row, a + hs_old, h->old_n, b + hs_new, h->new_n, scroll, max, cols, red, green, reset, emit);
    last_new = hs_new + h->new_n;
    printed = 1;
  }
  if (printed) {
    int after = last_new + DIFF_CONTEXT_LINES;
    if (after > bn) after = bn;
    shown += browse_context_lines(&row, b, last_new, after - last_new, scroll, max, cols, dim, reset, emit);
  }
  free(selected);
  trace_hunks_free(&hs);
  return shown;
}

int browse_edit_visual_count(repo_t * repo, unsigned long long id, int selected, int edit_i, int cols) {
  commit_t c = {0};
  commit_file_t * f = NULL;
  if (! browse_selected_file(repo, id, selected, &c, &f) || edit_i < 0 || edit_i >= f->edits.n) { commit_free(&c); return 0; }
  bytes_t old = {0}, new = {0};
  int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0, total = 0;
  if (! focused_diff_states(repo, id, f, &old, &old_type, &old_exists, &new, &new_type, &new_exists)) goto done;
  if (old_type == SOURCE_LINK || new_type == SOURCE_LINK || (old_exists && binary_like(&old)) || (new_exists && binary_like(&new))) total = 1;
  else {
    line_t * a = NULL, * b = NULL;
    int an = split_lines(&old, &a), bn = split_lines(&new, &b);
    if (an >= 0 && bn >= 0)
      total = browse_edit_diff_rows(&old, old_exists, a, an, &new, new_exists, b, bn, &f->edits.v[edit_i], 0, 0, cols, "", "", "", "", 0);
    free(a);
    free(b);
  }
done:
  free_bytes(&old);
  free_bytes(&new);
  commit_free(&c);
  return total;
}

void browse_emit_diff_line(int * row, int scroll, int max, int cols, const char * color, char prefix, line_t * line, const char * reset) {
  size_t n = line_chars(line), off = 0;
  int body = cols > 4 ? cols - 3 : 1;
  do {
    size_t chunk = n - off < (size_t) body ? n - off : (size_t) body;
    if (*row >= scroll && *row < scroll + max) {
      if (off) printf("%s  ", color);
      else printf("%s%c ", color, prefix);
      fwrite(line->s + off, 1, chunk, stdout);
      printf("%s\n", reset);
    }
    (*row)++;
    off += chunk;
  } while (off < n);
}

void browse_render_edit_diff(repo_t * repo, unsigned long long id, int selected, int edit_i, int scroll) {
  commit_t c = {0};
  commit_file_t * f = NULL;
  browse_clear();
  if (! browse_selected_file(repo, id, selected, &c, &f)) { printf("unknown commit/file\n"); fflush(stdout); return; }
  printf("%ssc browse: commit %llu file edit %d  up/down=scroll  left/backspace=back  q=quit%s\n\n",
    browse_color("\033[2m"), id, edit_i + 1, browse_color("\033[0m"));
  if (edit_i < 0 || edit_i >= f->edits.n) {
    printf("no edit selected\n");
    commit_free(&c);
    fflush(stdout);
    return;
  }
  bytes_t old = {0}, new = {0};
  int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0;
  if (! focused_diff_states(repo, id, f, &old, &old_type, &old_exists, &new, &new_type, &new_exists)) {
    printf("diff unavailable\n");
    commit_free(&c);
    fflush(stdout);
    return;
  }
  if (old_type == SOURCE_LINK || new_type == SOURCE_LINK || (old_exists && binary_like(&old)) || (new_exists && binary_like(&new))) {
    printf("binary/source summary: %zu bytes -> %zu bytes\n", old.n, new.n);
  } else {
    line_t * a = NULL, * b = NULL;
    int an = split_lines(&old, &a), bn = split_lines(&new, &b);
    int rows, cols;
    browse_size(&rows, &cols);
    int total = 0, max = browse_edit_body_rows(rows, cols, id, edit_i);
    const char * red = browse_color("\033[31m"), * green = browse_color("\033[32m"), * dim = browse_color("\033[2m"), * reset = browse_color("\033[0m");
    if (an >= 0 && bn >= 0)
      total = browse_edit_diff_rows(&old, old_exists, a, an, &new, new_exists, b, bn, &f->edits.v[edit_i], 0, max, cols, red, green, dim, reset, 0);
    printf("%sline %d-%d of %d%s\n", dim, total ? scroll + 1 : 0, scroll + max < total ? scroll + max : total, total, reset);
    if (an >= 0 && bn >= 0)
      browse_edit_diff_rows(&old, old_exists, a, an, &new, new_exists, b, bn, &f->edits.v[edit_i], scroll, max, cols, red, green, dim, reset, 1);
    free(a);
    free(b);
  }
  free_bytes(&old);
  free_bytes(&new);
  commit_free(&c);
  fflush(stdout);
}

int browse_input_ready(int ms) {
  fd_set fds;
  struct timeval tv;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

int browse_pending_key = 0;

int browse_key(void) {
  if (browse_pending_key) {
    int k = browse_pending_key;
    browse_pending_key = 0;
    return k;
  }
  unsigned char c = 0;
  if (read(STDIN_FILENO, &c, 1) != 1) return 0;
  if (c != 27) return c;
  if (! browse_input_ready(20) || read(STDIN_FILENO, &c, 1) != 1) return 27;
  if (c == '[') {
    char seq[32];
    int n = 0;
    while (n + 1 < (int) sizeof(seq) && browse_input_ready(2) && read(STDIN_FILENO, &seq[n], 1) == 1) {
      if (seq[n] >= '@' && seq[n] <= '~') { n++; break; }
      n++;
    }
    if (! n) return 27;
    char final = seq[n - 1];
    if (final == 'A') return 'k';
    if (final == 'B') return 'j';
    if (final == 'C') return '\r';
    if (final == 'D') return 127;
    return 27;
  }
  if (c == 'O' && browse_input_ready(2) && read(STDIN_FILENO, &c, 1) == 1) {
    if (c == 'A') return 'k';
    if (c == 'B') return 'j';
    if (c == 'C') return '\r';
    if (c == 'D') return 127;
  }
  return 27;
}

int browse_drain_motion(int k) {
  int steps = (k == 'j') ? 1 : (k == 'k' ? -1 : 0);
  while (steps && browse_input_ready(0)) {
    int next = browse_key();
    if (next == 'j') steps++;
    else if (next == 'k') steps--;
    else { browse_pending_key = next; break; }
    if (steps > 200) steps = 200;
    if (steps < -200) steps = -200;
  }
  return steps;
}

int browse_interactive(repo_t * repo) {
  ids_t chain = {0};
  struct termios oldt, raw;
  if (! read_chain(repo, &chain)) return 1;
  if (! chain.n) { ids_free(&chain); return 0; }
  if (tcgetattr(STDIN_FILENO, &oldt) != 0) { ids_free(&chain); return 1; }
  raw = oldt;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) { ids_free(&chain); return 1; }
  browse_screen_enter();
  int view = 0, commit_i = 0, file_i = 0, edit_i = 0, commit_off = 0, file_off = 0, edit_off = 0, diff_scroll = 0, dirty = 1, rc = 0;
  while (1) {
    if (dirty) {
      if (view == 0) browse_render_commits(repo, &chain, commit_i, commit_off);
      else if (view == 1) browse_render_files(repo, chain.v[commit_i], file_i, file_off);
      else if (view == 2) browse_render_file(repo, chain.v[commit_i], file_i, edit_i, edit_off);
      else browse_render_edit_diff(repo, chain.v[commit_i], file_i, edit_i, diff_scroll);
      dirty = 0;
    }
    int k = browse_key();
    if (k == 'q') break;
    int old_view = view, old_commit_i = commit_i, old_file_i = file_i, old_edit_i = edit_i;
    int old_commit_off = commit_off, old_file_off = file_off, old_edit_off = edit_off, old_diff_scroll = diff_scroll;
    int motion = browse_drain_motion(k);
    if (motion > 0) {
      if (view == 0 && commit_i + 1 < chain.n) {
        int step = motion < chain.n - 1 - commit_i ? motion : chain.n - 1 - commit_i;
        commit_i += step;
        if (commit_i >= commit_off + 20) commit_off = commit_i - 19;
      }
      else if (view == 1) {
        commit_t c = {0};
        int n = read_commit(repo, chain.v[commit_i], &c) ? c.n : 0;
        commit_free(&c);
        if (file_i + 1 < n) {
          int step = motion < n - 1 - file_i ? motion : n - 1 - file_i;
          file_i += step;
          if (file_i >= file_off + 20) file_off = file_i - 19;
        }
      } else if (view == 2) {
        commit_t c = {0};
        commit_file_t * f = NULL;
        int n = browse_selected_file(repo, chain.v[commit_i], file_i, &c, &f) ? f->edits.n : 0;
        commit_free(&c);
        if (edit_i + 1 < n) {
          int edit_rows = browse_file_edit_rows(chain.v[commit_i]);
          int step = motion < n - 1 - edit_i ? motion : n - 1 - edit_i;
          edit_i += step;
          if (edit_i >= edit_off + edit_rows) edit_off = edit_i - edit_rows + 1;
        }
      } else if (view == 3) {
        int rows, cols;
        browse_size(&rows, &cols);
        int n = browse_edit_visual_count(repo, chain.v[commit_i], file_i, edit_i, cols);
        int max = browse_edit_body_rows(rows, cols, chain.v[commit_i], edit_i);
        int room = n - max - diff_scroll;
        if (room > 0) diff_scroll += motion < room ? motion : room;
      }
    } else if (motion < 0) {
      int step = -motion;
      if (view == 0 && commit_i > 0) {
        if (step > commit_i) step = commit_i;
        commit_i -= step;
        if (commit_i < commit_off) commit_off = commit_i;
      }
      else if (view == 1 && file_i > 0) {
        if (step > file_i) step = file_i;
        file_i -= step;
        if (file_i < file_off) file_off = file_i;
      }
      else if (view == 2 && edit_i > 0) {
        if (step > edit_i) step = edit_i;
        edit_i -= step;
        if (edit_i < edit_off) edit_off = edit_i;
      }
      else if (view == 3 && diff_scroll > 0) {
        if (step > diff_scroll) step = diff_scroll;
        diff_scroll -= step;
      }
    } else if (k == '\r' || k == '\n') {
      if (view == 0) { view = 1; file_i = file_off = edit_i = edit_off = diff_scroll = 0; }
      else if (view == 1) { view = 2; edit_i = edit_off = diff_scroll = 0; }
      else if (view == 2) {
        commit_t c = {0};
        commit_file_t * f = NULL;
        int n = browse_selected_file(repo, chain.v[commit_i], file_i, &c, &f) ? f->edits.n : 0;
        commit_free(&c);
        if (n) { view = 3; diff_scroll = 0; }
      }
    } else if (k == 127 || k == 8) {
      if (view > 0) { view--; if (view < 3) diff_scroll = 0; }
    } else if (! k) {
      rc = 1;
      break;
    }
    dirty = view != old_view || commit_i != old_commit_i || file_i != old_file_i || edit_i != old_edit_i ||
      commit_off != old_commit_off || file_off != old_file_off || edit_off != old_edit_off || diff_scroll != old_diff_scroll;
    if (dirty && view == old_view && commit_off == old_commit_off && file_off == old_file_off &&
        diff_scroll == old_diff_scroll) {
      int patched = 0;
      if (view == 0) { browse_move_selection(3 + old_commit_i - commit_off, 3 + commit_i - commit_off); patched = 1; }
      else if (view == 1 && commit_i == old_commit_i) { browse_move_selection(3 + old_file_i - file_off, 3 + file_i - file_off); patched = 1; }
      else if (view == 2 && commit_i == old_commit_i && file_i == old_file_i && edit_off == old_edit_off) {
        int row = browse_file_edit_start_row(chain.v[commit_i]);
        browse_move_selection(row + old_edit_i - edit_off, row + edit_i - edit_off);
        patched = 1;
      } else if (view == 2 && commit_i == old_commit_i && file_i == old_file_i) {
        browse_render_file_edits(repo, chain.v[commit_i], file_i, edit_i, edit_off);
        patched = 1;
      }
      if (patched) dirty = 0;
    }
  }
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
  browse_screen_leave();
  fflush(stdout);
  ids_free(&chain);
  return rc;
}

int command_browse(repo_t * repo, int argc, char ** argv) {
  if (argc == 0 && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) return browse_interactive(repo);
  return browse_noninteractive(repo, argc, argv);
}

#define CLEAN_GONE 0
#define CLEAN_UNDONE 1
#define CLEAN_OLD 2

typedef struct {
  unsigned long long id;
  unsigned long long stored;
  char * path;
} clean_record_t;

typedef struct {
  int kind;
  char * path;
  unsigned long long stored;
  clean_record_t * records;
  int n;
  int size;
} clean_candidate_t;

typedef struct {
  clean_candidate_t * v;
  int n;
  int size;
} clean_candidates_t;

typedef struct {
  unsigned long long id;
  unsigned long long stored;
  char * path;
  bytes_t old;
  bytes_t new;
  int old_type;
  int new_type;
  int old_exists;
  int new_exists;
} clean_change_t;

typedef struct {
  clean_change_t * v;
  int n;
  int size;
} clean_changes_t;

const char * clean_kind_name(int kind) {
  if (kind == CLEAN_GONE) return "gone";
  if (kind == CLEAN_UNDONE) return "undone";
  return "old";
}

void clean_candidates_free(clean_candidates_t * cs) {
  for (int i = 0; i < cs->n; i++) {
    clean_candidate_t * c = &cs->v[i];
    free(c->path);
    for (int j = 0; j < c->n; j++) free(c->records[j].path);
    free(c->records);
  }
  free(cs->v);
  cs->v = NULL;
  cs->n = cs->size = 0;
}

void clean_change_free(clean_change_t * c) {
  free(c->path);
  free_bytes(&c->old);
  free_bytes(&c->new);
}

void clean_changes_free(clean_changes_t * cs) {
  for (int i = 0; i < cs->n; i++) clean_change_free(&cs->v[i]);
  free(cs->v);
  cs->v = NULL;
  cs->n = cs->size = 0;
}

int clean_record_push(clean_candidate_t * c, unsigned long long id, const char * path, unsigned long long stored) {
  if (c->n == c->size) {
    c->size = c->size ? 2 * c->size : INITIAL_SIZE;
    clean_record_t * v = realloc(c->records, c->size * sizeof(clean_record_t));
    if (! v) return 0;
    c->records = v;
  }
  c->records[c->n].id = id;
  c->records[c->n].stored = stored;
  c->records[c->n].path = xstrdup(path);
  if (! c->records[c->n].path) return 0;
  c->n++;
  c->stored += stored;
  return 1;
}

clean_candidate_t * clean_candidate_push(clean_candidates_t * cs, int kind, const char * path) {
  if (cs->n == cs->size) {
    cs->size = cs->size ? 2 * cs->size : INITIAL_SIZE;
    clean_candidate_t * v = realloc(cs->v, cs->size * sizeof(clean_candidate_t));
    if (! v) return NULL;
    cs->v = v;
  }
  clean_candidate_t * c = &cs->v[cs->n++];
  memset(c, 0, sizeof(*c));
  c->kind = kind;
  c->path = xstrdup(path);
  return c->path ? c : NULL;
}

clean_candidate_t * clean_candidate_find(clean_candidates_t * cs, int kind, const char * path) {
  for (int i = 0; i < cs->n; i++)
    if (cs->v[i].kind == kind && strcmp(cs->v[i].path, path) == 0) return &cs->v[i];
  return NULL;
}

int clean_candidate_add_record(clean_candidates_t * cs, int kind, const char * path, unsigned long long id, unsigned long long stored) {
  clean_candidate_t * c = clean_candidate_find(cs, kind, path);
  if (! c) c = clean_candidate_push(cs, kind, path);
  return c && clean_record_push(c, id, path, stored);
}

int clean_candidate_has_record(clean_candidate_t * c, unsigned long long id, const char * path) {
  for (int i = 0; i < c->n; i++)
    if (c->records[i].id == id && strcmp(c->records[i].path, path) == 0) return 1;
  return 0;
}

int clean_record_seen(clean_candidates_t * cs, unsigned long long id, const char * path) {
  for (int i = 0; i < cs->n; i++)
    for (int j = 0; j < cs->v[i].n; j++)
      if (cs->v[i].records[j].id == id && strcmp(cs->v[i].records[j].path, path) == 0) return 1;
  return 0;
}

int clean_path_has_move(repo_t * repo, ids_t * chain, const char * path) {
  for (int i = 0; i < chain->n; i++) {
    commit_t c = {0};
    if (! read_commit(repo, chain->v[i], &c)) return 1;
    for (int j = 0; j < c.n; j++) {
      commit_file_t * f = &c.files[j];
      if (f->kind == 3 && (strcmp(f->path, path) == 0 || (f->old_path && strcmp(f->old_path, path) == 0))) {
        commit_free(&c);
        return 1;
      }
    }
    commit_free(&c);
  }
  return 0;
}

int clean_change_push(clean_changes_t * cs, clean_change_t ch) {
  if (cs->n == cs->size) {
    cs->size = cs->size ? 2 * cs->size : INITIAL_SIZE;
    clean_change_t * v = realloc(cs->v, cs->size * sizeof(clean_change_t));
    if (! v) return 0;
    cs->v = v;
  }
  cs->v[cs->n++] = ch;
  return 1;
}

void clean_change_remove(clean_changes_t * cs, int i) {
  clean_change_free(&cs->v[i]);
  memmove(cs->v + i, cs->v + i + 1, (cs->n - i - 1) * sizeof(clean_change_t));
  cs->n--;
}

int clean_latest_pending(clean_changes_t * cs, const char * path) {
  for (int i = cs->n - 1; i >= 0; i--)
    if (strcmp(cs->v[i].path, path) == 0) return i;
  return -1;
}

int clean_change_from_file(repo_t * repo, index_t * idx, commit_t * c, commit_file_t * f, clean_change_t * out) {
  memset(out, 0, sizeof(*out));
  out->id = c->id;
  out->stored = f->stored_size;
  out->path = xstrdup(f->path);
  if (! out->path) return 0;
  int root_old = c->id == 0 || (c->parent == 0 && ! commit_exists(repo, 0));
  if (root_old) empty_bytes(&out->old);
  else if (! state_at_commit(repo, idx, f->path, c->parent, &out->old, &out->old_type, &out->old_exists)) return 0;
  if (! state_at_commit(repo, idx, f->path, c->id, &out->new, &out->new_type, &out->new_exists)) return 0;
  return out->old.data && out->new.data;
}

int clean_collect_undone(repo_t * repo, index_t * idx, ids_t * chain, clean_candidates_t * out) {
  clean_changes_t pending = {0};
  for (int p = chain->n - 1; p >= 0; p--) {
    commit_t c = {0};
    if (! read_commit(repo, chain->v[p], &c)) { clean_changes_free(&pending); return 0; }
    for (int i = 0; i < c.n; i++) {
      commit_file_t * f = &c.files[i];
      if (f->kind == 3 || index_find(idx, f->path) < 0 || clean_path_has_move(repo, chain, f->path)) continue;
      clean_change_t ch = {0};
      if (! clean_change_from_file(repo, idx, &c, f, &ch)) { clean_change_free(&ch); commit_free(&c); clean_changes_free(&pending); return 0; }
      int j = clean_latest_pending(&pending, ch.path);
      if (j >= 0 &&
          bytes_equal(&pending.v[j].old, pending.v[j].old_type, pending.v[j].old_exists, &ch.new, ch.new_type, ch.new_exists) &&
          bytes_equal(&pending.v[j].new, pending.v[j].new_type, pending.v[j].new_exists, &ch.old, ch.old_type, ch.old_exists)) {
        clean_candidate_t * cand = clean_candidate_push(out, CLEAN_UNDONE, ch.path);
        int ok = cand &&
          clean_record_push(cand, pending.v[j].id, pending.v[j].path, pending.v[j].stored) &&
          clean_record_push(cand, ch.id, ch.path, ch.stored);
        clean_change_remove(&pending, j);
        clean_change_free(&ch);
        if (! ok) { commit_free(&c); clean_changes_free(&pending); return 0; }
      } else if (! clean_change_push(&pending, ch)) {
        clean_change_free(&ch);
        commit_free(&c);
        clean_changes_free(&pending);
        return 0;
      }
    }
    commit_free(&c);
  }
  clean_changes_free(&pending);
  return 1;
}

int clean_collect_gone_and_old(repo_t * repo, index_t * idx, ids_t * chain, clean_candidates_t * out) {
  for (int p = chain->n - 1; p >= 0; p--) {
    commit_t c = {0};
    if (! read_commit(repo, chain->v[p], &c)) return 0;
    for (int i = 0; i < c.n; i++) {
      commit_file_t * f = &c.files[i];
      if (f->kind == 3 || clean_path_has_move(repo, chain, f->path)) continue;
      if (index_find(idx, f->path) < 0) {
        if (! clean_candidate_add_record(out, CLEAN_GONE, f->path, c.id, f->stored_size)) { commit_free(&c); return 0; }
        continue;
      }
      int later = 0;
      for (int q = p - 1; q >= 0 && ! later; q--) {
        commit_t newer = {0};
        if (! read_commit(repo, chain->v[q], &newer)) { commit_free(&c); return 0; }
        later = commit_file(&newer, f->path) != NULL;
        commit_free(&newer);
      }
      if (later && ! clean_record_seen(out, c.id, f->path)) {
        clean_candidate_t * cand = clean_candidate_push(out, CLEAN_OLD, f->path);
        if (! cand || ! clean_record_push(cand, c.id, f->path, f->stored_size)) { commit_free(&c); return 0; }
      }
    }
    commit_free(&c);
  }
  return 1;
}

int cmp_clean_candidates(const void * a, const void * b) {
  const clean_candidate_t * x = (const clean_candidate_t*) a;
  const clean_candidate_t * y = (const clean_candidate_t*) b;
  if (x->kind != y->kind) return x->kind - y->kind;
  if (x->stored != y->stored) return x->stored < y->stored ? 1 : -1;
  unsigned long long xi = x->n ? x->records[0].id : 0, yi = y->n ? y->records[0].id : 0;
  if (xi != yi) return xi < yi ? -1 : 1;
  return strcmp(x->path, y->path);
}

int collect_clean_candidates(repo_t * repo, index_t * idx, ids_t * chain, clean_candidates_t * out) {
  if (! clean_collect_undone(repo, idx, chain, out)) return 0;
  if (! clean_collect_gone_and_old(repo, idx, chain, out)) return 0;
  qsort(out->v, out->n, sizeof(clean_candidate_t), cmp_clean_candidates);
  return 1;
}

int clean_write_record(repo_t * repo, index_t * idx, FILE * fp, unsigned long long id, unsigned long long parent, commit_file_t * f) {
  if (f->kind == 3) return write_commit_move(fp, f->old_path, f->path, f->new_type, f->new_size, f->content_hash);
  bytes_t old = {0}, new = {0};
  edits_t edits = {0};
  int old_type = SOURCE_NONE, new_type = SOURCE_NONE, old_exists = 0, new_exists = 0, kind = 0;
  int root_old = id == 0 || (parent == 0 && ! commit_exists(repo, 0));
  if (root_old) empty_bytes(&old);
  else if (! state_at_commit(repo, idx, f->path, parent, &old, &old_type, &old_exists)) return 0;
  if (! state_at_commit(repo, idx, f->path, id, &new, &new_type, &new_exists)) { free_bytes(&old); return 0; }
  if (! old_exists && new_exists) kind = 1;
  else if (old_exists && ! new_exists) kind = 2;
  int ok = 1;
  if (! bytes_equal(&old, old_type, old_exists, &new, new_type, new_exists)) {
    ok = build_reverse_edits(kind, &old, &new, &edits) && write_commit_file(fp, f->path, kind, old_type, new_type, &old, &new, &edits);
  }
  edits_free(&edits);
  free_bytes(&old);
  free_bytes(&new);
  return ok;
}

int clean_write_commit_tmp(repo_t * repo, index_t * idx, commit_t * c, clean_candidate_t * cand, bytes_t * cur, int * cur_type, int * cur_exists) {
  char body[PATH_MAX], outp[PATH_MAX], buf[8192];
  snprintf(body, sizeof(body), "%s/commits/%016llx.clean-body", repo->sc, c->id);
  snprintf(outp, sizeof(outp), "%s/commits/%016llx.clean", repo->sc, c->id);
  FILE * body_fp = fopen(body, "wb");
  if (! body_fp) return 0;
  int changed = 0;
  for (int i = 0; i < c->n; i++) {
    commit_file_t * f = &c->files[i];
    if (f->kind != 3 && strcmp(f->path, cand->path) == 0) {
      bytes_t new = {0};
      int new_type = SOURCE_NONE, new_exists = 0, kind = 0;
      if (clean_candidate_has_record(cand, c->id, f->path)) {
        bytes_copy(&new, cur);
        new_type = *cur_type;
        new_exists = *cur_exists;
      } else if (! state_at_commit(repo, idx, cand->path, c->id, &new, &new_type, &new_exists)) {
        fclose(body_fp);
        unlink(body);
        return 0;
      }
      if (! bytes_equal(cur, *cur_type, *cur_exists, &new, new_type, new_exists)) {
        edits_t edits = {0};
        if (! *cur_exists && new_exists) kind = 1;
        else if (*cur_exists && ! new_exists) kind = 2;
        if (! build_reverse_edits(kind, cur, &new, &edits) ||
            ! write_commit_file(body_fp, cand->path, kind, *cur_type, new_type, cur, &new, &edits)) {
          edits_free(&edits);
          free_bytes(&new);
          fclose(body_fp);
          unlink(body);
          return 0;
        }
        edits_free(&edits);
        changed++;
      }
      free_bytes(cur);
      *cur = new;
      *cur_type = new_type;
      *cur_exists = new_exists;
    } else {
      if (! clean_write_record(repo, idx, body_fp, c->id, c->parent, f)) { fclose(body_fp); unlink(body); return 0; }
      changed++;
    }
  }
  fclose(body_fp);
  FILE * out = fopen(outp, "wb");
  FILE * in = fopen(body, "rb");
  if (! out || ! in) {
    if (out) fclose(out);
    if (in) fclose(in);
    unlink(body);
    unlink(outp);
    return 0;
  }
  fprintf(out, "SC1\ncommit %llu\nparent %llu\ntime %lld\nmessage %zu\n", c->id, c->parent, c->time, strlen(c->msg ? c->msg : ""));
  fwrite(c->msg ? c->msg : "", 1, strlen(c->msg ? c->msg : ""), out);
  fprintf(out, "\nfiles %d\n", changed);
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in))) fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  unlink(body);
  return 1;
}

unsigned long long clean_candidate_commit_bytes(repo_t * repo, clean_candidate_t * cand) {
  unsigned long long total = 0;
  for (int i = 0; i < cand->n; i++) {
    int seen = 0;
    for (int j = 0; j < i; j++) if (cand->records[j].id == cand->records[i].id) seen = 1;
    if (! seen) total += commit_stored_size(repo, cand->records[i].id);
  }
  return total;
}

int clean_rename_tmp(repo_t * repo, ids_t * chain, clean_candidate_t * cand) {
  (void) cand;
  for (int i = 0; i < chain->n; i++) {
    char tmp[PATH_MAX], cpath[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/commits/%016llx.clean", repo->sc, chain->v[i]);
    commit_path(repo, chain->v[i], cpath);
    if (rename(tmp, cpath) != 0) return 0;
  }
  return 1;
}

void clean_unlink_tmp(repo_t * repo, ids_t * chain) {
  for (int i = 0; i < chain->n; i++) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/commits/%016llx.clean", repo->sc, chain->v[i]);
    unlink(tmp);
    snprintf(tmp, sizeof(tmp), "%s/commits/%016llx.clean-body", repo->sc, chain->v[i]);
    unlink(tmp);
  }
}

int apply_clean_candidate(repo_t * repo, clean_candidate_t * cand, unsigned long long * reclaimed) {
  ids_t chain = {0};
  index_t idx = {0};
  bytes_t cur = {0}, head = {0};
  int cur_type = SOURCE_NONE, head_type = SOURCE_NONE, cur_exists = 0, head_exists = 0, ok = 0;
  *reclaimed = 0;
  if (! repo_clean(repo, "clean") || ! read_chain(repo, &chain) || ! load_index(repo, &idx)) goto done;
  unsigned long long before = clean_candidate_commit_bytes(repo, cand);
  empty_bytes(&cur);
  if (! state_at_commit(repo, &idx, cand->path, chain.v[0], &head, &head_type, &head_exists)) goto done;
  for (int p = chain.n - 1; p >= 0; p--) {
    commit_t c = {0};
    if (! read_commit(repo, chain.v[p], &c)) { commit_free(&c); goto done; }
    if (! clean_write_commit_tmp(repo, &idx, &c, cand, &cur, &cur_type, &cur_exists)) { commit_free(&c); goto done; }
    commit_free(&c);
  }
  if (! bytes_equal(&cur, cur_type, cur_exists, &head, head_type, head_exists)) goto done;
  if (! clean_rename_tmp(repo, &chain, cand)) goto done;
  unsigned long long after = clean_candidate_commit_bytes(repo, cand);
  *reclaimed = before > after ? before - after : cand->stored;
  ok = 1;
done:
  if (! ok) clean_unlink_tmp(repo, &chain);
  free_bytes(&cur);
  free_bytes(&head);
  ids_free(&chain);
  index_free(&idx);
  return ok;
}

void clean_render_list(clean_candidates_t * cs, int selected, int offset) {
  const char * dim = browse_color("\033[2m");
  const char * cyan = browse_color("\033[36m");
  const char * yellow = browse_color("\033[33m");
  const char * red = browse_color("\033[31m");
  const char * green = browse_color("\033[32m");
  const char * reset = browse_color("\033[0m");
  browse_clear();
  printf("%ssc clean: candidates  enter/right=open  c=clean  q=quit%s\n\n", dim, reset);
  for (int i = offset; i < cs->n && i < offset + 20; i++) {
    clean_candidate_t * c = &cs->v[i];
    char stored[32];
    format_bytes(c->stored, stored, sizeof(stored));
    const char * color = c->kind == CLEAN_GONE ? red : (c->kind == CLEAN_UNDONE ? green : yellow);
    browse_row(i == selected);
    printf("%s%-7s%s %s%-8s%s %3d  %s%s%s\n", color, clean_kind_name(c->kind), reset, yellow, stored, reset, c->n, cyan, c->path, reset);
  }
  fflush(stdout);
}

void clean_render_detail(clean_candidate_t * c, int offset) {
  const char * dim = browse_color("\033[2m");
  const char * cyan = browse_color("\033[36m");
  const char * yellow = browse_color("\033[33m");
  const char * reset = browse_color("\033[0m");
  char stored[32];
  format_bytes(c->stored, stored, sizeof(stored));
  browse_clear();
  printf("%ssc clean: %s  c=clean  left/backspace=back  q=quit%s\n\n", dim, clean_kind_name(c->kind), reset);
  printf("path: %s%s%s\n", cyan, c->path, reset);
  printf("stored: %s%s%s (%llu bytes)\n", yellow, stored, reset, c->stored);
  printf("records: %d\n\n", c->n);
  for (int i = offset; i < c->n && i < offset + 20; i++) {
    char size[32];
    format_bytes(c->records[i].stored, size, sizeof(size));
    printf("  commit %-6llu %-8s %s\n", c->records[i].id, size, c->records[i].path);
  }
  fflush(stdout);
}

int clean_confirm(clean_candidate_t * c) {
  char stored[32], answer[64];
  format_bytes(c->stored, stored, sizeof(stored));
  printf("clean %s %s (%s, %d records)\n", clean_kind_name(c->kind), c->path, stored, c->n);
  printf("this rewrites local source-control history and cannot be undone\n");
  printf("type \"clean\" to continue: ");
  fflush(stdout);
  if (! fgets(answer, sizeof(answer), stdin)) answer[0] = '\0';
  answer[strcspn(answer, "\r\n")] = '\0';
  return strcmp(answer, "clean") == 0;
}

int clean_interactive(repo_t * repo, clean_candidates_t * cs) {
  struct termios oldt, raw;
  if (tcgetattr(STDIN_FILENO, &oldt) != 0) return 1;
  raw = oldt;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return 1;
  browse_screen_enter();
  int view = 0, selected = 0, offset = 0, detail_off = 0, rc = 0;
  while (1) {
    if (view == 0) clean_render_list(cs, selected, offset);
    else clean_render_detail(&cs->v[selected], detail_off);
    int k = browse_key();
    if (k == 'q') break;
    if (k == 'j') {
      if (view == 0 && selected + 1 < cs->n) { selected++; if (selected >= offset + 20) offset++; }
      else if (view == 1 && detail_off + 20 < cs->v[selected].n) detail_off++;
    } else if (k == 'k') {
      if (view == 0 && selected > 0) { selected--; if (selected < offset) offset--; }
      else if (view == 1 && detail_off > 0) detail_off--;
    } else if (k == '\r' || k == '\n') {
      if (view == 0) { view = 1; detail_off = 0; }
    } else if (k == 127 || k == 8) {
      if (view == 1) view = 0;
    } else if (k == 'c') {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
      browse_screen_leave();
      unsigned long long reclaimed = 0;
      if (! clean_confirm(&cs->v[selected])) {
        printf("clean cancelled\n");
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) { rc = 1; break; }
        browse_screen_enter();
        continue;
      }
      if (! apply_clean_candidate(repo, &cs->v[selected], &reclaimed)) {
        fprintf(stderr, "clean failed\n");
        rc = 1;
      } else {
        char size[32];
        format_bytes(reclaimed, size, sizeof(size));
        printf("cleaned %s %s: %d records, %s reclaimed\n", clean_kind_name(cs->v[selected].kind), cs->v[selected].path, cs->v[selected].n, size);
      }
      return rc;
    } else if (! k) {
      rc = 1;
      break;
    }
  }
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
  browse_screen_leave();
  fflush(stdout);
  return rc;
}

int command_clean(repo_t * repo, int argc, char ** argv) {
  (void) argv;
  if (argc != 0) { fprintf(stderr, "usage: sc clean\n"); return 1; }
  if (! isatty(STDIN_FILENO) || ! isatty(STDOUT_FILENO)) { fprintf(stderr, "sc clean requires an interactive terminal\n"); return 1; }
  if (! repo_clean(repo, "clean")) return 1;
  ids_t chain = {0};
  index_t idx = {0};
  clean_candidates_t candidates = {0};
  int rc = 1;
  if (! read_chain(repo, &chain) || ! load_index(repo, &idx) || ! collect_clean_candidates(repo, &idx, &chain, &candidates)) goto done;
  if (! candidates.n) { printf("nothing to clean\n"); rc = 0; goto done; }
  rc = clean_interactive(repo, &candidates);
done:
  clean_candidates_free(&candidates);
  index_free(&idx);
  ids_free(&chain);
  return rc;
}

int command_log(repo_t * repo, int argc, char ** argv) {
  char filter_buf[PATH_MAX], *filter = NULL;
  if (argc > 1) { fprintf(stderr, "usage: sc log [path]\n"); return 1; }
  if (argc == 1) {
    if (rel_path(repo, argv[0], filter_buf)) filter = filter_buf;
    else { snprintf(filter_buf, sizeof(filter_buf), "%s", argv[0]); filter = filter_buf; }
  }
  unsigned long long head = 0;
  if (! read_head_id(repo, &head)) return 0;
  while (1) {
    commit_t c = {0};
    if (! read_commit(repo, head, &c)) return 1;
    if (filter) {
      if (! commit_file(&c, filter)) {
        if (head == 0 || (c.parent == 0 && ! commit_exists(repo, 0))) { commit_free(&c); break; }
        head = c.parent;
        commit_free(&c);
        continue;
      }
    }
    time_t tt = (time_t) c.time;
    char * ts = ctime(&tt);
    if (ts) ts[strcspn(ts, "\n")] = '\0';
    print_log_msg(head, ts ? ts : "unknown time", c.msg ? c.msg : "");
    if (head == 0 || (c.parent == 0 && ! commit_exists(repo, 0))) { commit_free(&c); break; }
    head = c.parent;
    commit_free(&c);
  }
  return 0;
}

int remove_tree(const char * path) {
  struct stat st;
  if (lstat(path, &st) != 0) return 0;
  if (! S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path) == 0;
  DIR * dir = opendir(path);
  if (! dir) return 0;
  struct dirent * de;
  int ok = 1;
  while ((de = readdir(dir))) {
    if (de->d_name[0] == '.' && (! de->d_name[1] || (de->d_name[1] == '.' && ! de->d_name[2]))) continue;
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
    ok = remove_tree(child) && ok;
  }
  closedir(dir);
  return rmdir(path) == 0 && ok;
}

int command_destroy(int argc, char ** argv) {
  (void) argv;
  if (argc != 0) { fprintf(stderr, "usage: sc destroy\n"); return 1; }
  if (! is_dir(SC_DIR)) { fprintf(stderr, "no local .source-control repository in this directory\n"); return 1; }
  char cwd[PATH_MAX], path[PATH_MAX], answer[64];
  if (! getcwd(cwd, sizeof(cwd))) return 1;
  snprintf(path, sizeof(path), "%s/%s", cwd, SC_DIR);
  printf("delete: %s\n", path);
  printf("this removes local history, staged data, index, config, and commits\n");
  printf("type \"destroy\" to continue: ");
  fflush(stdout);
  if (! fgets(answer, sizeof(answer), stdin)) answer[0] = '\0';
  answer[strcspn(answer, "\r\n")] = '\0';
  if (strcmp(answer, "destroy") != 0) {
    printf("destroy cancelled\n");
    return 1;
  }
  if (! remove_tree(SC_DIR)) { fprintf(stderr, "destroy failed\n"); return 1; }
  printf("destroyed %s\n", path);
  return 0;
}

char * shell_quote(const char * s) {
  size_t n = 3;
  for (const char * p = s; *p; p++) n += *p == '\'' ? 4 : 1;
  char * out = malloc(n);
  if (! out) return NULL;
  char * q = out;
  *q++ = '\'';
  for (const char * p = s; *p; p++) {
    if (*p == '\'') { memcpy(q, "'\\''", 4); q += 4; }
    else *q++ = *p;
  }
  *q++ = '\'';
  *q = '\0';
  return out;
}

int command_ok(const char * cmd) {
  int st = system(cmd);
  return st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int command_capture(const char * cmd, bytes_t * out) {
  memset(out, 0, sizeof(*out));
  FILE * fp = popen(cmd, "r");
  if (! fp) return 0;
  size_t cap = 0;
  unsigned char buf[4096];
  while (1) {
    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (n) {
      if (out->n + n > cap) {
        cap = cap ? cap * 2 : 8192;
        while (cap < out->n + n) cap *= 2;
        unsigned char * data = realloc(out->data, cap);
        if (! data) { pclose(fp); free_bytes(out); return 0; }
        out->data = data;
      }
      memcpy(out->data + out->n, buf, n);
      out->n += n;
    }
    if (n < sizeof(buf)) {
      if (ferror(fp)) { pclose(fp); free_bytes(out); return 0; }
      break;
    }
  }
  int st = pclose(fp);
  return st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

char * command_text(const char * cmd) {
  bytes_t b = {0};
  if (! command_capture(cmd, &b)) return NULL;
  char * out = malloc(b.n + 1);
  if (! out) { free_bytes(&b); return NULL; }
  memcpy(out, b.data, b.n);
  out[b.n] = '\0';
  while (b.n && (out[b.n - 1] == '\n' || out[b.n - 1] == '\r')) out[--b.n] = '\0';
  free_bytes(&b);
  return out;
}

char * git_command(const char * dir, const char * args) {
  char * q = shell_quote(dir);
  if (! q) return NULL;
  size_t n = strlen(q) + strlen(args) + 16;
  char * cmd = malloc(n);
  if (cmd) snprintf(cmd, n, "git -C %s %s", q, args);
  free(q);
  return cmd;
}

int git_capture(const char * dir, const char * args, bytes_t * out) {
  char * cmd = git_command(dir, args);
  int ok = cmd && command_capture(cmd, out);
  free(cmd);
  return ok;
}

char * git_text(const char * dir, const char * args) {
  char * cmd = git_command(dir, args);
  char * out = cmd ? command_text(cmd) : NULL;
  free(cmd);
  return out;
}

int git_ok(const char * dir, const char * args) {
  char * cmd = git_command(dir, args);
  int ok = cmd && command_ok(cmd);
  free(cmd);
  return ok;
}

int paths_find(paths_t * ps, const char * path) {
  for (int i = 0; i < ps->n; i++)
    if (strcmp(ps->v[i], path) == 0) return i;
  return -1;
}

int git_paths(const char * worktree, paths_t * paths) {
  bytes_t b = {0};
  if (! git_capture(worktree, "ls-files -z", &b)) return 0;
  size_t start = 0;
  for (size_t i = 0; i <= b.n; i++) {
    if (i == b.n || b.data[i] == '\0') {
      if (i > start) {
        char * p = xstrndup((char*) b.data + start, i - start);
        int ok = p && paths_push(paths, p);
        free(p);
        if (! ok) { free_bytes(&b); return 0; }
      }
      start = i + 1;
    }
  }
  free_bytes(&b);
  return 1;
}

int import_stage_snapshot(repo_t * repo, const char * worktree, paths_t * git_files) {
  index_t idx = {0};
  stage_t stage = {0};
  paths_t tracked = {0};
  int ok = load_index(repo, &idx) && load_stage(repo, &stage);
  for (int i = 0; ok && i < git_files->n; i++) {
    char full[PATH_MAX], spath[PATH_MAX];
    struct stat st;
    bytes_t b = {0};
    int type = SOURCE_NONE;
    if (strncmp(git_files->v[i], SC_DIR "/", strlen(SC_DIR) + 1) == 0 || strcmp(git_files->v[i], SC_DIR) == 0) continue;
    path_join(full, sizeof(full), worktree, git_files->v[i]);
    if (! read_source(full, &b, &type) || lstat(full, &st) != 0) {
      fprintf(stderr, "skipping unsupported Git entry: %s\n", git_files->v[i]);
      free_bytes(&b);
      continue;
    }
    staged_path(repo, git_files->v[i], spath);
    stage_entry_t se = {
      hash_string(git_files->v[i]), hash_bytes(b.data, b.n), b.n, st.st_mtime, type,
      STAGE_ADD, NULL, xstrdup(git_files->v[i])
    };
    if (! se.path || ! write_bytes(spath, b.data, b.n)) {
      free(se.path);
      ok = 0;
    } else if (! stage_set(&stage, se)) {
      free(se.path);
      ok = 0;
    } else if (! paths_push(&tracked, git_files->v[i])) {
      ok = 0;
    }
    free_bytes(&b);
  }
  for (int i = 0; ok && i < idx.n; i++) {
    if (paths_find(&tracked, idx.v[i].path) >= 0) continue;
    stage_entry_t se = {idx.v[i].path_hash, 0, 0, 0, SOURCE_NONE, STAGE_DEL, NULL, xstrdup(idx.v[i].path)};
    ok = se.path && stage_set(&stage, se);
    if (! ok) free(se.path);
  }
  ok = ok && save_stage(repo, &stage);
  paths_free(&tracked);
  stage_free(&stage);
  index_free(&idx);
  return ok;
}

int git_commit_info(const char * root, const char * hash, long long * t, char ** msg) {
  char args[256];
  snprintf(args, sizeof(args), "show -s --format=%%at%%n%%B %s", hash);
  char * out = git_text(root, args);
  if (! out) return 0;
  char * nl = strchr(out, '\n');
  if (! nl) { free(out); return 0; }
  *nl = '\0';
  *t = atoll(out);
  *msg = xstrdup(nl + 1);
  free(out);
  if (! *msg) return 0;
  size_t n = strlen(*msg);
  while (n && ((*msg)[n - 1] == '\n' || (*msg)[n - 1] == '\r')) (*msg)[--n] = '\0';
  return 1;
}

int parse_commit_list(char * text, paths_t * commits) {
  for (char * p = text; *p;) {
    char * e = strchr(p, '\n');
    if (e) *e = '\0';
    if (*p && ! paths_push(commits, p)) return 0;
    if (! e) break;
    p = e + 1;
  }
  return 1;
}

int import_confirm(const char * root, const char * branch, int n) {
  char answer[64];
  printf("import Git history into source-control\n");
  printf("git root: %s\n", root);
  printf("destination: %s/%s\n", root, SC_DIR);
  printf("branch: %s\n", branch && strcmp(branch, "HEAD") != 0 ? branch : "detached HEAD");
  printf("commits: %d first-parent commits\n", n);
  printf("type \"import\" to continue: ");
  fflush(stdout);
  if (! fgets(answer, sizeof(answer), stdin)) answer[0] = '\0';
  answer[strcspn(answer, "\r\n")] = '\0';
  return strcmp(answer, "import") == 0;
}

int command_import(int argc, char ** argv) {
  (void) argv;
  if (argc != 0) { fprintf(stderr, "usage: sc import\n"); return 1; }
  char * root = command_text("git rev-parse --show-toplevel 2>/dev/null");
  if (! root || ! root[0]) { free(root); fprintf(stderr, "not inside a Git repository\n"); return 1; }
  char final[PATH_MAX], importing[PATH_MAX], tmp_template[PATH_MAX];
  snprintf(final, sizeof(final), "%s/%s", root, SC_DIR);
  snprintf(importing, sizeof(importing), "%s/%s.importing", root, SC_DIR);
  if (path_exists(final)) { fprintf(stderr, "%s already exists\n", final); free(root); return 1; }
  if (path_exists(importing)) { fprintf(stderr, "%s already exists\n", importing); free(root); return 1; }
  bytes_t status = {0};
  if (! git_capture(root, "status --porcelain", &status)) { free(root); return 1; }
  if (status.n) { fprintf(stderr, "sc import requires a clean Git working tree\n"); free_bytes(&status); free(root); return 1; }
  free_bytes(&status);
  char * branch = git_text(root, "rev-parse --abbrev-ref HEAD 2>/dev/null");
  char * list = git_text(root, "rev-list --first-parent --reverse HEAD 2>/dev/null");
  paths_t commits = {0};
  if (! branch || ! list || ! parse_commit_list(list, &commits) || ! commits.n) {
    fprintf(stderr, "no Git commits to import\n");
    free(branch); free(list); free(root); paths_free(&commits);
    return 1;
  }
  if (! import_confirm(root, branch, commits.n)) {
    printf("import cancelled\n");
    free(branch); free(list); free(root); paths_free(&commits);
    return 1;
  }
  snprintf(tmp_template, sizeof(tmp_template), "/tmp/sc-import-XXXXXX");
  char * worktree = mkdtemp(tmp_template);
  if (! worktree) { perror("mkdtemp"); free(branch); free(list); free(root); paths_free(&commits); return 1; }
  char * qroot = shell_quote(root), * qwork = shell_quote(worktree);
  size_t cmd_n = (qroot && qwork) ? strlen(qroot) + strlen(qwork) + 96 : 0;
  char * cmd = cmd_n ? malloc(cmd_n) : NULL;
  int rc = 1, made_import = 0, made_worktree = 0;
  repo_t repo;
  memset(&repo, 0, sizeof(repo));
  snprintf(repo.root, sizeof(repo.root), "%s", root);
  snprintf(repo.sc, sizeof(repo.sc), "%s", importing);
  snprintf(repo.pattern, sizeof(repo.pattern), "%s", DEFAULT_MATCH);
  if (! qroot || ! qwork || ! cmd) goto done;
  snprintf(cmd, cmd_n, "git -C %s worktree add --detach %s HEAD >/dev/null 2>/dev/null", qroot, qwork);
  if (! command_ok(cmd)) goto done;
  made_worktree = 1;
  if (! init_repo_at(importing)) goto done;
  made_import = 1;
  for (int i = 0; i < commits.n; i++) {
    char args[128];
    paths_t files = {0};
    long long t = 0;
    char * msg = NULL;
    snprintf(args, sizeof(args), "checkout -q %s", commits.v[i]);
    if (! git_ok(worktree, args) || ! git_paths(worktree, &files) ||
        ! import_stage_snapshot(&repo, worktree, &files) ||
        ! git_commit_info(root, commits.v[i], &t, &msg)) {
      paths_free(&files);
      free(msg);
      goto done;
    }
    index_t idx = {0};
    stage_t stage = {0};
    int ok = load_index(&repo, &idx) && load_stage(&repo, &stage) &&
      commit_stage(&repo, &idx, &stage, t, msg, "import", 1, 1) == 0;
    index_free(&idx);
    stage_free(&stage);
    paths_free(&files);
    free(msg);
    if (! ok) goto done;
  }
  if (rename(importing, final) != 0) goto done;
  made_import = 0;
  printf("imported %d Git commits into %s\n", commits.n, final);
  rc = 0;
done:
  if (made_worktree && cmd) {
    snprintf(cmd, cmd_n, "git -C %s worktree remove --force %s >/dev/null 2>/dev/null", qroot, qwork);
    command_ok(cmd);
  } else if (worktree) remove_tree(worktree);
  if (made_import) remove_tree(importing);
  free(cmd);
  free(qroot);
  free(qwork);
  free(branch);
  free(list);
  free(root);
  paths_free(&commits);
  if (rc) fprintf(stderr, "import failed\n");
  return rc;
}

void print_help(void) {
  printf(
    "sc: local restorable source history\n"
    "\n"
    "usage:\n"
    "  sc init\n"
    "  sc add [paths...]\n"
    "  sc ignore [--literal|--glob|--regex] <paths...>\n"
    "  sc rm <paths...>\n"
    "  sc mv <old> <new>\n"
    "  sc status\n"
    "  sc ls [--tracked] [--untracked] [--ignored]\n"
    "  sc diff [--full] [path]\n"
    "  sc diff [--full] --staged [path]\n"
    "  sc diff [--full] --latest [path]\n"
    "  sc diff [--full] COMMIT [--against BASE]\n"
    "  sc diff [--full] --against BASE [COMMIT]\n"
    "  sc commit -m \"message\"\n"
    "  sc amend [-m \"message\"]\n"
    "  sc log [path]\n"
    "  sc browse [COMMIT] [FILE]\n"
    "  sc clean\n"
    "  sc trace <path> [--lines A[:B]] [--regex REGEX] [START..END|--after COMMIT|--before COMMIT]\n"
    "  sc squash COMMIT|HEAD|START..END|ALL\n"
    "  sc restore COMMIT\n"
    "  sc revert [--no-warning] <path>\n"
    "  sc import\n"
    "  sc destroy\n"
    "\n"
    "actions:\n"
    "  init          create .source-control in this directory\n"
    "  add           track explicit files; scan directories for matches\n"
    "  ignore        skip files or directory trees in future scans\n"
    "  rm            remove tracked files and record deletion\n"
    "  mv            move a tracked file and record a tiny rename\n"
    "  status        show tracked changes and first 20 matching untracked files\n"
    "  ls            show up to 10 tracked and 10 untracked files\n"
    "  ls --tracked  show every tracked file, newest edited first\n"
    "  ls --untracked  show every matching untracked file\n"
    "  ls --ignored  show every ignored or skipped path\n"
    "  diff          show local, staged, latest, or historical changes\n"
    "  commit        save staged changes; auto-stage extras only when nothing is staged\n"
    "  amend         fold staged/tracked/new-file edits into HEAD; -m replaces message\n"
    "  log           show commits newest first; optional path filters\n"
    "  browse        inspect stored history bytes by commit and file\n"
    "  clean         interactively remove least-relevant stored history records\n"
    "  trace         show commits and hunks that touched a file or selected lines\n"
    "  squash        combine one commit, HEAD, a range, or ALL\n"
    "  restore       commit a restore to COMMIT's state\n"
    "  revert        discard staged or unstaged changes to one file\n"
    "  import        convert current Git first-parent history into .source-control\n"
    "  destroy       delete only this directory's .source-control data\n"
    "\n"
    "examples:\n"
    "  sc init\n"
    "  sc add src include\n"
    "  sc ignore build scratch.tmp\n"
    "  sc ignore '.DerivedData*'\n"
    "  sc ignore --regex '{.}[.]DerivedData.*'\n"
    "  sc ls --untracked\n"
    "  sc ls --ignored\n"
    "  sc diff src/main.c\n"
    "  sc diff --staged\n"
    "  sc diff --latest\n"
    "  sc diff 2\n"
    "  sc diff 2 --against 1\n"
    "  sc diff --full src/main.c\n"
    "  sc status\n"
    "  sc commit -m \"parse config\"\n"
    "  sc mv old/file.c new/file.c\n"
    "  sc amend -m \"parse config\"\n"
    "  sc browse HEAD source_control.c\n"
    "  sc clean\n"
    "  sc trace src/main.c --lines 20:25\n"
    "  sc trace src/main.c --regex \"parse.*config\"\n"
    "  sc squash HEAD\n"
    "  sc squash 2..4\n"
    "  sc squash ALL\n"
    "  sc restore 4\n"
    "  sc revert --no-warning src/main.c\n"
    "  sc import\n"
  );
}

int main(int argc, char ** argv) {
  if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "help") == 0) {
    print_help();
    return 0;
  }
  if (strcmp(argv[1], "init") == 0) return init_repo();
  if (strcmp(argv[1], "destroy") == 0) return command_destroy(argc - 2, argv + 2);
  if (strcmp(argv[1], "import") == 0) return command_import(argc - 2, argv + 2);
  if (strcmp(argv[1], "push") == 0) { fprintf(stderr, "unsupported: no remote\n"); return 1; }
  if (strcmp(argv[1], "branch") == 0) { fprintf(stderr, "reserved for later\n"); return 1; }
  repo_t repo;
  if (! find_repo(&repo)) {
    fprintf(stderr, "no .source-control repository found\n");
    return 1;
  }
  if (strcmp(argv[1], "add") == 0) return command_add(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "ignore") == 0) return command_ignore(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "rm") == 0) return command_rm(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "mv") == 0) return command_mv(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "status") == 0) return command_status(&repo);
  if (strcmp(argv[1], "ls") == 0) return command_ls(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "diff") == 0) return command_diff(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "commit") == 0) return command_commit(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "amend") == 0) return command_amend(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "log") == 0) return command_log(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "browse") == 0) return command_browse(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "clean") == 0) return command_clean(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "trace") == 0) return command_trace(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "squash") == 0) return command_squash(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "restore") == 0) return command_restore(&repo, argc - 2, argv + 2);
  if (strcmp(argv[1], "revert") == 0) return command_revert(&repo, argc - 2, argv + 2);
  fprintf(stderr, "unknown command: %s\n", argv[1]);
  return 1;
}
