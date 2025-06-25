// for reference

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

size_t dir_entry_cnt = 0;
DIR *p_cur_dir = 0;
char *cur_dir_name = 0;

int is_dir(char *name) {
  struct stat buf;
  if (!name || stat(name, &buf) )
    return 0;
  return S_ISDIR(buf.st_mode);
}

int is_file(char *name) {
  struct stat buf;
  if (!name || stat(name, &buf) )
    return 0;
  return S_ISREG(buf.st_mode);
}

int is_exe(char *name) {
  struct stat buf;
  if (!name || stat(name, &buf) )
    return 0;
  return buf.st_mode & S_IXUSR
#ifdef S_IXGRP
      || buf.st_mode & S_IXGRP
#endif
#ifdef S_IXOTH
      || buf.st_mode & S_IXOTH
#endif
  ;
}

// ls directory to list ? array?





