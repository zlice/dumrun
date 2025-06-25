#include "font.c" // ctype dirent math stdio string stdint sys/stat

#include <fcntl.h> // shm types
#include <errno.h> // general error handling
#include <getopt.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>


//////////////
// global vars
//////////////

XEvent xev;
Window xwin;
GC gc;
Display *disp = 0;
Visual *xvis;
XImage *ximg;
XEvent event;
Atom xa_win_type;
Atom xa_motif_hints;

int arg_keep_alive = 0, arg_decor = 0, arg_center = 0,
    arg_width = 0, arg_height = 0, typing_args = 0,
    running = 1, cfgd = 0;

#define MAX_INPUT_SZ 1024
// for a small stupid program, 1024 characters is plenty
char input_txt[MAX_INPUT_SZ + 1] = {0}; // the chars typed
     // typed input is left of input_pos, predict is right
int txt_xstarts[MAX_INPUT_SZ + 1] = {0}, // x draw pos
    input_exe_pos = 0, // after last '/', for getting exe to predict
    input_pos = 0, // actually typed up until this pos
    draw_pos = 0,  // drawn/predicted until this pos
    last_input_pos = 0, // for draw tracking
    last_draw_pos = 0;
uint32_t arg_bg_color = 0xff000000, arg_fg_color = 0xffffffff,
         *surf_pixels; // black and white defaults (endian?)

int shift_pressed = 0, ctrl_pressed = 0;//, alt_pressed = 0;

#define MAX_PATH_NAMES_SZ 1024
#define MAX_PATH_CNT      256
#define MAX_EXE_NAMES_SZ 300000 // 300k - some OSes can dupe paths
#define MAX_EXE_CNT       30000 // 30k
// on my distro, everything is really in /bin
// ~3k in /bin
// ~33k chars + nulls
// 10k and 100k should be overkill, my system dupes PATHs so triple?

// a lot of this is 'arena' and 'slicing' - see 'font.c' crawl for more
//
// build path list with PATH split by ':'.
// each path has an id, used by the exe list.
// exe list has 'starts' and 'parent-id' list.
// once PATH and children exe lists are set up
// the rest of the arena is for custom full
// paths typed in the box. although i assume
// most uses will be a simple 'runthis' name
char parent_path_names[MAX_PATH_NAMES_SZ], // 'arena' of parent paths
     path_exe_names[MAX_EXE_NAMES_SZ], // 'arena' of path exe names
     *parent_path_starts[MAX_PATH_CNT],
     *path_exe_starts[MAX_EXE_CNT],
     *predict_starts[MAX_EXE_CNT], // str* in exe_starts, swaps around every typed char
     *nxt_path = parent_path_names,
     *nxt_exe = path_exe_names;
int path_cnt = 0,
         env_path_end = 0, // behind here is PATH, save it
         exe_cnt = 0,
         predict_idx = 0,  // while tabbing
         predict_cnt = 0,  // total predicts, these reset every typed char
         parent_ids[MAX_EXE_CNT]; // index into parent_path_starts[]

DIR *DIR_path;
struct stat file_chk;
struct dirent *ddir = 0;

/////////////////////////////////////////////
// path/exe adding, prediction and fork (run)
/////////////////////////////////////////////

int is_input_exec() { // if input_txt is a exec file
  int ret = (stat(input_txt, &file_chk) == 0 && S_ISREG(file_chk.st_mode)
      && file_chk.st_mode & S_IXUSR) ? 1 : 0;

  // if input_txt does not start with path, crawl PATHs
  if (ret == 0) {
    for (int path_chk = 0 ; path_chk < env_path_end ; path_chk++) {
      int path_len = strlen(parent_path_starts[path_chk]);
      char path_exe[input_pos + path_len + 3];
      strcpy(path_exe, parent_path_starts[path_chk]);
      path_exe[path_len] = '/';
      strcpy(path_exe + path_len + 1, input_txt);
      path_exe[path_len + input_pos + 1] = 0;
      ret |= (stat(path_exe, &file_chk) == 0 && S_ISREG(file_chk.st_mode)
          && file_chk.st_mode & S_IXUSR) ? 1 : 0;
      if (ret)
        break;
    } // for PATHs
  }
  return ret;
} // is_input_exec

int add_dir() {
  int nxt_e_len = 0, exe_chk = exe_cnt,
      nxt_p_len = input_pos != 0 ? input_pos : (int)strlen(input_txt) + 1;
  char *in_end = input_txt + nxt_p_len, *in_pos = input_txt,
        in_cpy[in_end - in_pos + 1];
  uint32_t in_idx = 0;

  // this should only be possible with a padded PATH var
  if (nxt_path + nxt_p_len - parent_path_names > MAX_PATH_NAMES_SZ) {
    fprintf(stderr, "Max path size reached. Not adding directory!\n"
                    "Check your PATH, it must be too big.\n");
    return 0;
  }

  for ( ; in_idx < in_end - in_pos ; in_idx++)
    in_cpy[in_idx] = input_txt[in_idx];
  in_cpy[in_idx] = 0; // stack not wiped to 0s

  DIR_path = opendir(in_cpy);
  if (DIR_path == 0) { // these things happen in PATH
    fprintf(stdout, "Skipping PATH directory, cannot find or read: "
                     "%s\n", input_txt);
    return 0;
  }

  parent_path_starts[path_cnt] = nxt_path;
  while (in_pos < in_end) { // strcpy by 'slice'
    *nxt_path = *in_pos;
    nxt_path++; in_pos++;
  } // parent_path_names[x] = dir
  *++nxt_path = 0;

  while ( (ddir = readdir(DIR_path) ) != 0) {
    nxt_e_len = _D_EXACT_NAMLEN(ddir) + 1;
    if (nxt_e_len + nxt_exe - path_exe_names > MAX_EXE_NAMES_SZ) {
      fprintf(stderr, "Too many executables in your paths!\n"
                      "Not adding more. Check %s for files.\n",input_txt);
      exe_cnt = exe_chk;
      return 0;
    }
    if (ddir->d_name[0] == '.')
      continue; // skip self ref and up .. (and hidden)
    strcpy(nxt_exe, ddir->d_name);
    path_exe_starts[exe_cnt] = nxt_exe;
    nxt_exe += nxt_e_len + 1;
    parent_ids[exe_cnt++] = path_cnt;
  } // while read dir

  closedir(DIR_path);

  if (exe_chk == exe_cnt) // nothing in dir, do not add it
    return 0;

  path_cnt++;
  return 1;
} // add_dir

int pop_dir() { // remove last dir
  if (path_cnt <= env_path_end)
    return 0; // keep PATH loaded
  const int cur_exe_parent = parent_ids[exe_cnt - 1];
  while (parent_ids[--exe_cnt] == cur_exe_parent)
    { }
  exe_cnt++; // add back the lost -- above
  nxt_exe = path_exe_starts[exe_cnt];
  nxt_path = parent_path_starts[--path_cnt];
  return 1;
} // pop_dir

void add_env_path() { // split ':', loop add_dir
  char *env_path, *env_start, *env_idx,
       *in_pos = input_txt, *env_end;

  env_path = getenv("PATH");
  if (env_path == 0) {
    fprintf(stderr, "CANNOT FIND 'PATH' var! good luck...\n");
    return;
  }
  env_idx = env_start = env_path;
  env_end = env_path + strlen(env_path);

  for (; env_idx <= env_end ; env_idx++, in_pos++) {
    *in_pos = *env_idx; // strcpy
    if (*in_pos == ':' || *in_pos == 0) {
      if (in_pos - input_txt > 1) { // :: or / in PATH
        *in_pos = 0;
        env_path_end += add_dir();
        while (in_pos >= input_txt)
          *in_pos-- = 0;
      } else {
        in_pos--;
      }
    } // if : or 0
  } // for PATH
} // add_env_path

void del_predict_txt() {
  while (draw_pos > input_pos)
    input_txt[draw_pos--] = 0;
  input_txt[draw_pos] = 0;
} // del_predict_txt

// tabs through matching predicts and does the actual draws
void tabcomplete(int do_cycle) {
  if (predict_cnt == 0)
    return; // nothing to predict

  int only_word = predict_idx, adv_pos = 0;

  if (do_cycle == 1) { // manually tab through list
    predict_idx++;
    if (predict_idx >= predict_cnt)
      predict_idx = 0;
  } else if (do_cycle == 2) {
    predict_idx--;
    if (predict_idx > predict_cnt) // underflow
      predict_idx = predict_cnt - 1;
  }

  char *pre_txt = predict_starts[predict_idx]
                  + (input_pos - input_exe_pos),
       *in_txt = input_txt;

  while (pre_txt[adv_pos] != 0) {
    in_txt[input_pos+adv_pos] = pre_txt[adv_pos];
    adv_pos++;
  }

  draw_pos = input_pos + adv_pos;
  if (do_cycle && only_word == predict_idx)
    input_pos = draw_pos;
} // tabcomplete

void autocomplete() {
  if (typing_args)
    return; // not predicting args

  char *cmp_n, *cmp_e,
       *name_start = input_txt + input_exe_pos,
       *name_end   = input_txt + input_pos;
  int exe_idx = exe_cnt - 1, path_start, path_end;

  path_start = path_end = path_cnt - 1; // asume last/custom path
  if (path_end == env_path_end - 1)
    path_start = 0; // all PATH dirs

  predict_cnt = predict_idx = 0;

  // search exe names and build pointer list to them (for tabbing)
  // start from end because we don't know where
  // each 'parent' is in the middle of the exe list
  // TODO: sort names would be nice, but extra complexity
  while (exe_idx >= 0 && path_start <= path_end
         && predict_cnt < MAX_EXE_CNT) {
    cmp_n = name_start;
    cmp_e = path_exe_starts[exe_idx];
    while (*cmp_n && cmp_n < name_end && *cmp_e && *cmp_n == *cmp_e)
      { cmp_n++ ; cmp_e++; } // case sensitive str compare
    if (cmp_n == name_end)
      predict_starts[predict_cnt++] = path_exe_starts[exe_idx];
    path_end = parent_ids[--exe_idx];
  }

  if (predict_cnt) // something predicted
    tabcomplete(0);
} // autocomplete


//////////
// drawing
//////////

void draw_img() {
  XPutImage(disp, xwin, gc, ximg, 0, 0, 0, 0, arg_width, arg_height);
}

// may be a better way to do this? but works for now
// draws input_txt[]
void draw_input() {
  if (draw_pos >= MAX_INPUT_SZ)
    return;
  if (draw_pos < input_pos)
    draw_pos = input_pos;

  int c, // really 'char' but used as [idx]
      is_predict = 0, // if 'cur_pos' > input_pos
      dst_start_y = (arg_height / 2) - (font_max_height / 2), // center
      dst_end_x = 0, // char width or end of draw
      src_x = 0, src_y = 0, dst_start_x = 0, dst_x = 0, dst_y = 0,
      start_pos = input_pos, end_pos = draw_pos, over_offset = 0;
      // font_max_height already set

  // check for overdraw by setting xstarts for whole input_txt
  for (int s_pos = 0, x_start = 0 ; s_pos <= draw_pos ; s_pos++) {
    txt_xstarts[s_pos] = x_start;
    c = input_txt[s_pos];
    if (c != 0) // 0 is used as 'space'
      c -= 32; // the ascii start of 32, 0x20, 'space' is subtracted
    x_start += bmap_widths[c];
  }

  // find range to draw
  if (txt_xstarts[draw_pos] >= arg_width) {
    dst_start_x = arg_width;
    start_pos = draw_pos; // unset above if-chain pos checks
    // rewind to find first txt on screen (could calc if monospace was enforced)
    //for (start_pos = draw_pos - 1 ; start_pos >= 0 ; start_pos--) {
    for (; start_pos >= 0 ; start_pos--) {
      c = input_txt[start_pos];
      if (c != 0)
        c -= 32;
      dst_start_x -= bmap_widths[c];
      if (dst_start_x <= 0)
        break;
    }
    start_pos++; // want the character that over-drew
    over_offset = txt_xstarts[start_pos]; // offset draws
  } // if over-draw / scroll draw
  else if (input_pos < last_input_pos || draw_pos < last_draw_pos) {
    // clear right - backspace / tab smaller text
    start_pos = input_pos;
    end_pos = last_draw_pos;
    if (draw_pos < last_draw_pos) // overdrawn predicted text
      start_pos = 0;
    else if (start_pos != 0) // typed input starts before input_pos
      start_pos--;
  } else if (last_input_pos < input_pos) // draw new char - may have new predict
    start_pos = last_input_pos;
  // else - nop - regular draw - fill right - tab bigger text

  // draw/clear range that needs updated
  //for ( ; start_pos <= end_pos ; start_pos++) {
  // ???? off by 1 on <= ???
  for ( ; start_pos < end_pos ; start_pos++) {
    int clear_right = 0;

    dst_start_x = txt_xstarts[start_pos] - over_offset;

    c = input_txt[start_pos];
    if (c != 0)
      c -= 32;

    dst_end_x = dst_start_x + bmap_widths[c];

    if (dst_end_x > arg_width || (start_pos == end_pos && over_offset)
        || (start_pos >= draw_pos && start_pos <= last_draw_pos) ) {
      clear_right = 1;
      dst_end_x = txt_xstarts[end_pos] + bmap_widths[c];
      if (dst_end_x > arg_width)
        dst_end_x = arg_width;
    }

    is_predict = clear_right == 0 && start_pos > input_pos - 1 ? 1 : 0;

    for (src_y = 0, dst_y = dst_start_y ; src_y < font_max_height ; src_y++, dst_y++) {
      for (src_x = 0, dst_x = dst_start_x ; dst_x < dst_end_x ; src_x++, dst_x++) {
        uint32_t *src_pix = is_predict != 0 ? font_bmap_invt : font_bmap_norm;
        if (!clear_right) { // assuming top left pixel of 'space' is 0 for logic
          src_pix += bmap_cstarts[c];
          src_pix += src_y * bmap_widths[c] + src_x;
        }
        surf_pixels[dst_y * arg_width + dst_x] = *src_pix != 0 ? *src_pix
                         : is_predict != 0 ? arg_fg_color : arg_bg_color;
      } // for x
    } // for y

    if (clear_right) {
      dst_start_x = dst_end_x;
      break;
    }

    dst_start_x += bmap_widths[0]; // 'cursor'
  } // for draw range

  last_input_pos = input_pos;
  last_draw_pos = draw_pos;

  draw_img();
} // draw_input

void keybd_handle_key() {
  int mods = event.xkey.state;
  xkb_keysym_t keysym = XLookupKeysym(&event.xkey, 0);
  KeySym k;
  char c = (char)xkb_keysym_to_utf32(keysym),
       capc;
  XLookupString(&event.xkey, &capc, 1, &k, 0);

  shift_pressed = mods & ShiftMask ? 1 : 0;
  ctrl_pressed = mods & ControlMask ? 1 : 0;
  //alt_pressed = mods & Mod1Mask ? 1 : 0;

  if (c >= 0x20 && c <= 0x7E && input_pos < MAX_INPUT_SZ) {
    // XLookupKeysym does translate Ctrl+c/d/l
    if (ctrl_pressed) {
      if (c == 'c' || c == 'd' || c == 'l') {
      //if (capc == 0x3 || capc == 0x4 || capc == 0xc) {
        input_pos = input_exe_pos = typing_args = 0;
        del_predict_txt();
        while (pop_dir() )
          { }
      }
      goto end_keys;
    } // TODO: alt d, ctrl w

    if (typing_args) { // anything's allowed
      input_txt[input_pos++] = capc;
      goto end_keys;
    }

    if (c == ' ') {
      input_pos = draw_pos;

      if (is_input_exec() == 1 || input_pos == 0) {
        typing_args = 1;
        predict_cnt = predict_idx = 0; // not predicting anymore
        input_txt[input_pos++] = capc;
      } // else - autocomplete dir only
      goto end_keys;
    } // is space

    del_predict_txt();

    input_txt[input_pos++] = capc;

    if (c == '/') {
      if ((input_pos > 1 && input_txt[input_pos-1] == '/'
          && input_txt[input_pos-2] == '/') // no doubles '//'
            || add_dir() != 1) { // only valid directories
        input_txt[--input_pos] = 0;
        return;
      }
      input_exe_pos = input_pos;
    }

    autocomplete();

    if (predict_cnt == 0) {
      input_txt[--input_pos] = 0;
      del_predict_txt();
      if (last_draw_pos == draw_pos)
        return; // repeating invalid, nothing to update
    } // invalid non-predict char
  // end if regular char
  } else if (keysym == XKB_KEY_Tab || keysym == XKB_KEY_ISO_Left_Tab) {
    if (input_pos == 0)
      return; // autocomplete compare '0' causes chaos

    if (predict_cnt == 0) // happens on backspace
      autocomplete();
    else {
      del_predict_txt();
      tabcomplete(1 + (keysym == XKB_KEY_ISO_Left_Tab || shift_pressed ? 1 : 0) );
    }
  } else if (keysym == XKB_KEY_Return) {
    // typing_args implies start is exec file
    // check if input or autocomplete is exec
    const int bk_input = input_pos;
    int chk_is_exec = is_input_exec();
    if (!chk_is_exec) {
      input_pos = draw_pos; // tmp autocomplete chk
      chk_is_exec = is_input_exec();
      if (!chk_is_exec)
        input_pos = bk_input;
    }
    if (chk_is_exec || typing_args) {
      if (fork() != 0) // failed or parent side fork
        goto diedie;

      char *shell = getenv("SHELL");
      if (!shell)
        shell = "/bin/sh";

      setsid();
      execl(shell, shell, "-c", input_txt, 0);
diedie:
      running = 0;
      return;
    } else if (predict_cnt)
      input_pos = draw_pos; // accept autocomplete
    else
      return; // nothing to update
  } else if (keysym == XKB_KEY_BackSpace) {
    if (input_pos == 0)
      return; // nothing to backspace

    if (predict_cnt || ctrl_pressed) { // clear all
      if (ctrl_pressed) {
        if (typing_args) { // go to last space ' '
          typing_args = input_pos - 1;
          while (typing_args > 0 && input_txt[--typing_args] != ' ')
            { }
          if (typing_args != 0) // after space
            typing_args++;
          input_pos = typing_args;
        } else // clear all
          input_pos = input_exe_pos = 0;
      } else // clear predict
        predict_cnt = predict_idx = 0;
    } else { // regular backspace
      if (input_txt[input_pos - 1] == '/') {
        pop_dir();
        input_exe_pos = input_pos - 1;
        while (input_exe_pos > 0 && input_txt[--input_exe_pos] != '/')
          { }
      } else if (input_txt[input_pos - 1] == ' ') {
        typing_args = input_pos - 1;
        while (typing_args > 0 && input_txt[--typing_args] != ' ')
          { }
        if (typing_args != 0)
          typing_args++;
      }

      input_pos--;

      if (input_txt[input_exe_pos] == '/')
        input_exe_pos++;
    } // regular backspace

    if (input_pos == 0) {
      input_exe_pos = typing_args = 0;
      while (pop_dir() )
        { }
    }
    del_predict_txt(); // ensures previous txt 0'd
  // end backspace
  } else if (keysym == XKB_KEY_Escape) {
    fprintf(stdout, "playing sudoku\n");
    running = 0;
    return;
  } else // if chain key
    return; // don't re-draw nothing (mod keys)

end_keys:
  draw_input();
} // keybd_handle_key


int main(int argc, char ** argv) {
  fprintf(stdout, "start...\n");

  int opt;
  while ((opt = getopt(argc, argv, "hdckb:f:z:F:H:W:")) != -1) {
    switch (opt) {
     case 'd': arg_decor = 1; break;
     case 'c': arg_center = 1; break;
     case 'k': arg_keep_alive = 1; break;
     case 'b': arg_bg_color = strtoul(optarg, 0, 0); break;
       // note: there's not a great way to detect errors here
       // so bad values are 0 which is pure transparency
       // TODO: improve, prepend alpha maybe? prepend 0x? allow r,g,b?
       //       sub # with 0x ?
     case 'f':
       arg_fg_color = strtoul(optarg, 0, 0); break;
     case 'z':
       arg_font_sz = atoi(optarg); break; // see font.c for default
     case 'F':
       arg_fontname = optarg; break;
     case 'H':
       arg_height = atoi(optarg); break;
     case 'W':
       arg_width = atoi(optarg); break;
     case 'h':
     default: // would love '-center' or '-pos(xy)' but wayland, zwlr_layer_shell_v1 anchors best you can do
       fprintf(stdout, "usage: %s [-hkbfpzFHW]\n"
       "\t-h : this message\n"
       "\t-d : request server side decorations\n"
       "\t-c : center position (not multi-monitor aware)\n"
       "\t-k : keep window alive if it loses focus\n"
       "\t-b : background color in 0xAARRGGBB hex value (uses stroul, default: 0xFF000000 black)\n"
       "\t-f : text color in 0xAARRGGBB hex value (alpha-rgb, default: 0xFFFFFFFF white)\n"
       "\t-z : font point size (default: 16)\n"
       "\t-F : font name (searches directories in /usr/share/fonts)\n"
       "\t-H : window height (default/minimum is set by font size. rest is padding)\n"
       "\t-W : window width (default: ridiculous)\n" , argv[0]);
       if (opt != 'h')
         exit(0);
    } // switch opt
  } // while getopts
  // this is needed to set minimum height
  gen_font_bmap(arg_bg_color, arg_fg_color);
  add_env_path();

  // arbitrary range checks, vivisble but not over a 4k monitor
  if (arg_height < 10 || arg_height > 5000)
    arg_height = font_max_height;
  if (arg_width < 10 || arg_width > 5000)
    arg_width = font_total_width; // ridiculous, offscreen depending on font size

  disp = XOpenDisplay(0);

  if (disp == 0) {
    fprintf(stderr, "Cannot connect to display server!\n");
    return 1;
  }

  xwin = XCreateSimpleWindow(disp, DefaultRootWindow(disp), 0, 0,
             arg_width, arg_height, 0, arg_bg_color, arg_fg_color);
  xvis = DefaultVisual(disp, 0);
  XSelectInput(disp, xwin, ExposureMask | KeyPressMask
             | FocusChangeMask | StructureNotifyMask);
  XMapWindow(disp, xwin);
  XFlush(disp);
  gc = XCreateGC(disp, xwin, 0, 0);

  // wait for map
  while(event.type != MapNotify)
    XNextEvent(disp, &event);

  // center window
  if (arg_center)
    XMoveWindow(disp, xwin,
      (DisplayWidth(disp, 0) / 2) - (arg_width / 2),
      (DisplayHeight(disp, 0) / 2) - (arg_height / 2) );

  long hints[5] = {arg_decor ? 0 : 2, 0, 0, 0, 0}, jnk = 0;
  xa_win_type = XInternAtom(disp, "_NET_WM_WINDOW_TYPE", False);
  XChangeProperty(disp, xwin, xa_win_type, XA_ATOM,
                  32, PropModeReplace, (unsigned char*) &jnk, 1);

  xa_motif_hints = XInternAtom(disp, "_MOTIF_WM_HINTS", False);

  XChangeProperty(disp, xwin, xa_motif_hints, xa_motif_hints,
            32, PropModeReplace, (unsigned char *)&hints, 5);

  // set title
  XStoreName(disp, xwin, "dumrun");

  ///////////////////////// create surface buffer
  const int size = arg_width * 4 * arg_height; // 4 = ARGB

  surf_pixels = malloc(size);

  for (int n = 0; n < arg_width * arg_height ; ++n)
    surf_pixels[n] = arg_bg_color; // init to bg color

  ximg = XCreateImage(disp, xvis, 24, ZPixmap, 0, (char*)surf_pixels,
         arg_width, arg_height, 32, 0);

  ///////////////////////// start main loop

  while (running) {
    XNextEvent(disp, &event);

    if (event.type == Expose)
      draw_img();
    else if (event.type == KeyPress)
      keybd_handle_key();
    else if (event.type == FocusOut && arg_keep_alive == 0)
      running = 0;
  }

  fprintf(stdout, "exiting...\n");

  // shutdown
  free(surf_pixels);
  free(font_bmap_norm);
  free(font_bmap_invt);
  fprintf(stdout, "freed everything! bye!\n");
  return 0;
} // main

// MIT License
//
// Copyright (c) 2025 zlice
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
