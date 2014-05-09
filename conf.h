/* See LICENSE for licence details. */
/* dri device */
static const char *dri_path = "/dev/dri/card0";

/* shell */
const char *shell_cmd = "/bin/bash";
// const char *shell_cmd = "/bin/csh"; /* for BSD */

/* TERM value */
const char *term_name = "yaft-256color"; /* default TERM */

/* color: index number of color_palette[] in color.h */
enum {
	DEFAULT_FG = 7,
	DEFAULT_BG = 0,
	ACTIVE_CURSOR_COLOR = 2,
	PASSIVE_CURSOR_COLOR = 1,
};

/* misc */
enum {
	DEBUG = false,             /* write dump of input to stdout, debug message to stderr */
	TABSTOP = 8,               /* hardware tabstop */
	LAZY_DRAW = false,         /* reduce drawing when input data size is larger than BUFSIZE */
	SUBSTITUTE_HALF = 0xFFFD,  /* used for missing glyph(single width): SPACE (U+20) */
	SUBSTITUTE_WIDE = 0x3013,  /* used for missing glyph(double width): GETA MARK (U+3000) */
	REPLACEMENT_CHAR = 0xFFFD, /* used for malformed UTF-8 sequence: REPLACEMENT CHARACTER (U+FFFD) */
};
