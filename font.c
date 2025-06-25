#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#define as_argb(a, r, g, b) \
  ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

// 0,0 is top left
uint32_t *font_bmap_norm, // alloc'd to [HEIGHT][WIDTH]
         *font_bmap_invt,
         bmap_cstarts[96] = {0}, // offset into ^bmap for chars[x][y]
         bmap_widths[96] = {0}; // see ascii below, 127 - 32 = 95 + null
int font_max_height = 0, font_max_width = 0,
    font_total_width = 0, // used for default window sizes
    arg_font_sz = 16;
char   *arg_fontname = "DejavuSansMono"; // had to pick 1. should be on most OS?

// colors
float gray = 0, bg_r, bg_g, bg_b, fg_r, fg_g, fg_b;
uint8_t bg_a, fg_a;
uint32_t g_bg_color, g_fg_color, pix_color = 0;

//void draw_bitmap(FT_Bitmap* bitmap, FT_Int x_off, FT_Int y_off, int idx) {
void draw_bitmap(FT_Bitmap* bitmap, uint32_t x_off, uint32_t y_off, int idx) {
  //FT_Int dst_x, dst_y, src_x, src_y;
  uint32_t dst_x, dst_y, src_x, src_y;

  // bitmap->pixel_mode is assumed to be 'gray'
  // so we blend each RGB % based on that, but use provided alpha
  // since this is a gray-scale, i thin you have to use decimals

  for (dst_x = x_off, src_x = 0 ; src_x < bitmap->width ; dst_x++, src_x++) {
    for (dst_y = y_off, src_y = 0 ; src_y < bitmap->rows ; dst_y++, src_y++) {
      //if (dst_x < 0 || dst_y < 0 || dst_x >= bmap_widths[idx] || dst_y >= font_max_height)
      if (dst_x >= bmap_widths[idx] || dst_y >= (uint32_t)font_max_height)
        continue;

      int dst_xy = bmap_cstarts[idx];
      dst_xy += dst_y * bmap_widths[idx] + dst_x; // font[c][yx], [yx] is [y][x]

      gray = (float)bitmap->buffer[src_y * bitmap->width + src_x] / 255.0; // 127 / 255 ~ 49.8%

      pix_color = g_bg_color;
      if (gray != 0.0)
        pix_color = as_argb(fg_a, (gray * fg_r * 255),
                                  (gray * fg_g * 255),
                                  (gray * fg_b * 255) );

      *(font_bmap_norm + dst_xy) = pix_color; // font_bmap_norm[c][yx]

      // invert for highlighting, easy but twice as much memory
      pix_color = g_fg_color;
      if (gray != 0.0)
        pix_color = as_argb(bg_a, (gray * bg_r * 255),
                                  (gray * bg_g * 255),
                                  (gray * bg_b * 255) );

      *(font_bmap_invt + dst_xy) = pix_color;
    } // for x
  } // for y
} // draw_bitmap

int gen_font_bmap(uint32_t bg_color, uint32_t fg_color) {
  FT_Library    library;
  FT_Face       face;

  FT_GlyphSlot  slot;
  FT_Matrix     matrix = { 0xffff, 0x0, 0x0, 0xffff };
  // 0 degree transformation matrix
  // you HAVE to define these - their actual math is
  //  .xx = (FT_Fixed)( cos(0) * 0x10000L),
  //  .xy = (FT_Fixed)(-sin(0) * 0x10000L),
  //  .yx = (FT_Fixed)( sin(0) * 0x10000L),
  //  .yy = (FT_Fixed)( cos(0) * 0x10000L)
  //};
  FT_Vector     pen;    /* untransformed origin */
  FT_Error      error;

  char          *ascii = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
  // space = 32 aka 0x20, to about 127 which is ~

  int n /* loop var */, num_chars = 95; // strlen(ascii);

  error = FT_Init_FreeType(&library);
  if (error) {
    fprintf(stderr, "FAILED TO INIT FREETYPE\n");
    exit(1);
  }

  // note: could use fontconfig but we have to crawl directories
  //       for programs anyway. so, why not do it for fonts too?

  // the highest full path on my system is 74 characters
  char font_filepath[256]; // tmp swap and final file
  struct stat chk;
  int match = 0;

  // find full path of font
  if (arg_fontname[0] != '/') { // given a full path
    DIR *DIR_crawl;
    char *filename, *cmp1, *cmp2;
    struct dirent *fdir = 0;
    // short of recursion, keep a push/pop stack of full font paths
    //
    // names - has /usr/share/fonts/[]/usr/share/fonts/path1[]/usr/share/fonts/path2[]...
    // starts - points to begining of each 'names' entry (0, 18, 40, etc)
    // idx - tracks the curernt name/starts/sz

    // this will grow with depth going into directories
    // then shrink when you go back up a level
    // files searches are exhausted first before going into dirs

    // // 'arena' of directory names
    char crawl_list_names[1024] = "/usr/share/fonts/"; // max was 279 on my box, only 2 dir deep
    size_t cur_dir_idx = 0, nxt_len = 0,
           cur_len = strlen(crawl_list_names);
    char *crawl_list_starts[256], *cur_dir = crawl_list_names,
         *nxt_dir = cur_dir + cur_len + 1, added_directories = 0;
    crawl_list_starts[0] = cur_dir;

    while (!match) {
      added_directories = 0;
      cur_dir = crawl_list_starts[cur_dir_idx];
      strcpy(font_filepath, cur_dir);
      DIR_crawl = opendir(font_filepath); // TODO:WARN: this WILL lead to crash if unreadable
      cur_len = strlen(font_filepath);

      while ( (fdir = readdir(DIR_crawl) ) ) {
        filename = cmp1 = fdir->d_name;
        if (filename[0] == '.' && (filename[1] == '.' || filename[1] == 0) )
          continue; // skip self and '..' up, lest search for infinity
        nxt_len = cur_len + strlen(filename);
        if (nxt_len > 250) {
          fprintf(stderr, "THIS FONT PATH SHOULD NOT BE BIGGER THAN 100 CHARS!\n"
                          "IT IS BIGGER THAN 250! WHAT ARE YOU DOING?\n"
                          "Font path in question : %s + %s\nEXITING!!!\n",
                 font_filepath, filename);
          exit(1);
        }
        strcpy(font_filepath+cur_len, filename);
        stat(font_filepath, &chk);
        if (S_ISREG(chk.st_mode) ) { // is file
          cmp2 = arg_fontname;
          cmp1 = filename;
          while (*cmp2 && *cmp1 && toupper(*cmp1) == toupper(*cmp2) )
            { cmp2++ ; cmp1++; } // str compare
          if (*cmp2 == 0 && (*cmp1 == 0 || *cmp1 == '.') )
            { match = 1; break; } // '.' file extension assumes match
            // TODO: implement 'closest' match ???
        } else if (S_ISDIR(chk.st_mode) ) { // add to crawl
          if (nxt_dir + nxt_len - crawl_list_names > 1024) {
            fprintf(stderr, "WARN: too many directories in /usr/share/fonts !\n"
                            "Paths will be skipped! Occurred at : %s", nxt_dir);
            continue;
          }
          if (!added_directories) {
            nxt_dir = cur_dir; // backup and overwrite self with own directories
            if (cur_dir_idx != 0)
              cur_dir_idx--;
          }
          strcpy(nxt_dir, font_filepath);
          cur_dir = nxt_dir;
          nxt_dir += nxt_len;
          *(nxt_dir++) = '/';
          *(nxt_dir++) = 0;
          cur_dir_idx++;
          crawl_list_starts[cur_dir_idx] = cur_dir;
          added_directories = 1;
          // cur_len not changed until next while(!match)
        }
      } // while crawl dir
      closedir(DIR_crawl);
      if (!added_directories)
        cur_dir_idx--;
      if (cur_dir_idx <= 0)
        break; // not found
    } // while !match
  } // if arg_fontname was not full path
  else {
    strcpy(font_filepath, arg_fontname);
    match = 1;
  }

  fprintf(stdout, "got font file path : %s\n", font_filepath);
  stat(font_filepath, &chk);
  if (!match || ! S_ISREG(chk.st_mode) ) {
    fprintf(stderr, "Font not found!\nTry using a different font, "
                    "full path, check spelling, or check that "
                    "DejavuSansMono is installed on your system.\n");
    exit(1);
  }

  error = FT_New_Face(library, font_filepath, 0, &face); // create face object
  if (error) {
    fprintf(stderr, "FAILED TO MAKE FACE\n");
    exit(1);
  }

  error = FT_Set_Char_Size(face, arg_font_sz * 64, 0, 100, 0); // set character size
  if (error) {
    fprintf(stderr, "Unable to create font!!!\n");
    exit(1);
  }

  slot = face->glyph;

  pen.x = pen.y = 0; // top left draw position (26.6 cartesian space)

  //////////////// get font dimensions
  // a lil weird, especially with variable width fonts, which are =) the worst
  // 'width' and 'rows' are the pixels of width and height
  // however some characters (" " space, "-" dash) have less or no width/height
  // the cursor still needs to 'advance' though, which will take place of width
  // slot->advance.x >> 6 (1/64th unit in font points)
  // alternative 'total width' is the 'left' offset position, and 'width' of pixels
  // slot->bitmap_left + slot->bitmap.width;
  // height works similarly, some 'heights' and 'tops' are small or negative

  FT_Set_Transform(face, &matrix, &pen); // only needs done once for dimensions
  float f_tot_w = 0, f_width = 0, f_height = 0;
  int nxt_start = 0;

  for (n = 0; n < num_chars; n++) { // first loop gets per-char dimensions
    error = FT_Load_Char(face, ascii[n], FT_LOAD_RENDER);
    if (error)
      continue; // ignore errors

    f_height = slot->bitmap.rows;
    f_width = slot->bitmap_left + slot->bitmap.width;

    if (f_width == 0) // if ((slot->advance.x >> 6) > f_width)
      f_width = slot->advance.x >> 6; // space may be 0 left 0 width, but SHOULD have width

    bmap_widths[n] = (uint32_t)f_width;

    if ((int)f_height > font_max_height)
      font_max_height = (int)f_height;

    f_tot_w += slot->advance.x; // advance is what you want. bounding boxes are different
  } // for overall dimensions

  // add 2 pixel padding
  font_max_height += 2;

  // now that we have max_height, calculate the start positions
  // this allows variable-widths to fit into a mono-height array
  // chop up font_bmap[totalpixelcount] from font_bmap[y][x] to per character
  // this is kind of a font_bmap[c][y][x] or [c][yx]
  // font[c] and each c is a variable sized-x [y][x] with y always max_height
  for (n = 0; n < num_chars; n++) {
    bmap_cstarts[n] = nxt_start;
    nxt_start += bmap_widths[n] * font_max_height;
  } // for cstarts

  font_total_width = (uint32_t)f_tot_w >> 6; // 1/64th of float font widths

  font_bmap_norm = calloc(font_max_height * font_total_width, sizeof(uint32_t) );
  font_bmap_invt = calloc(font_max_height * font_total_width, sizeof(uint32_t) );
  // used as (font_bmap + xystart) [char_Y][char_X]

  //////////////// draw the font

  // set these (once) before the actual draw
  g_bg_color = bg_color;
  g_fg_color = fg_color;
  bg_r = ((bg_color >> 16) & 255) / 255.0;
  bg_g = ((bg_color >>  8) & 255) / 255.0;
  bg_b = (bg_color & 255)         / 255.0;
  bg_a = bg_color >> 24;
  fg_r = ((fg_color >> 16) & 255) / 255.0;
  fg_g = ((fg_color >>  8) & 255) / 255.0;
  fg_b = (fg_color & 255)         / 255.0;
  fg_a = fg_color >> 24;

  //const int y_center_offset = ((font_max_height / 3) * 2) + ((font_max_height % 3) * 2);
  const int y_center_offset = (font_max_height / 3) + (font_max_height / 2);
  // 2/3 of max height + remainder from round down of 2/3 math
  // this may not be exact for non-monospace, need to test

  for (n = 0; n < num_chars; n++) {
    FT_Set_Transform(face, &matrix, &pen); // move the pen portion

    // let freetype do its thing. loads glyph over previous
    error = FT_Load_Char(face, ascii[n], FT_LOAD_RENDER);
    if (error) // silent on errors but shouldn't happen unless font's bad?
      continue;

    draw_bitmap(&slot->bitmap, slot->bitmap_left,
          y_center_offset - slot->bitmap_top, n);

  } // for build font bitmap

  FT_Done_Face(face);
  FT_Done_FreeType(library);

  return 0;
} // gen_font_bmap
