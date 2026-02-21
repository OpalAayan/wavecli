// wave.c — Terminal wave visualizer
// Double-buffered, signal-safe, with CLI arguments and color themes.
//
// Copyright (c) 2026. MIT License.

#define WAVE_VERSION "1.0.0"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// ════════════════════════════════════════════════════════════════════
//  Constants
// ════════════════════════════════════════════════════════════════════

#define MAX_BYTES_PER_CELL 30     // ANSI escape + UTF-8 glyph + reset
#define FRAME_BUF_PADDING 256     // extra headroom for frame buffer
#define STARFIELD_DENSITY 600     // 1-in-N chance of a star per cell
#define STARFIELD_GRAY_BASE 236   // base 256-color grayscale index
#define STARFIELD_GRAY_RANGE 4    // number of gray shades available
#define FRAME_COLOR_DIVISOR 200.0 // frame counter → color phase divisor
#define WAVE_COLOR_OFFSET 0.18    // per-wave color phase offset
#define TWO_PI 6.2831853071795864

#define DEFAULT_FPS 60
#define DEFAULT_NUM_WAVES 5
#define DEFAULT_SPEED 1.0
#define DEFAULT_PALETTE "rainbow"

#define MIN_FPS 1
#define MAX_FPS 240
#define MIN_WAVES 1
#define MAX_WAVES 50

#define EXIT_OK 0
#define EXIT_ERR 1
#define EXIT_OOM 2

// ════════════════════════════════════════════════════════════════════
//  Types & Data
// ════════════════════════════════════════════════════════════════════

typedef struct {
  double freq;
  double amp;
  double phase_spd;
  const char *glyph;
} Wave;

typedef struct {
  double speed_mult;
  int fps;
  int num_waves;
  const char *color_name;
  const char *glyph; // NULL = use per-wave defaults
} WaveConfig;

// ── Palette entry ──────────────────────────────────────────────────
typedef int (*palette_fn)(double t);

typedef struct {
  const char *name;
  palette_fn fn;
} Palette;

// ════════════════════════════════════════════════════════════════════
//  Globals for signal handlers (minimal — async-signal-safe only)
// ════════════════════════════════════════════════════════════════════

static volatile sig_atomic_t g_resized = 1; // force initial read
static volatile sig_atomic_t g_quit = 0;

// Resources tracked globally so cleanup is centralized
static char *g_frame_buf = NULL;
static int *g_fb = NULL;
static double *g_fbval = NULL;
static Wave *g_waves = NULL;
static double *g_phase = NULL;

// ════════════════════════════════════════════════════════════════════
//  Error handling helpers
// ════════════════════════════════════════════════════════════════════

__attribute__((format(printf, 1, 2))) static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "\033[1;31merror:\033[0m ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(EXIT_ERR);
}

static void die_oom(const char *context) {
  fprintf(stderr, "\033[1;31mfatal:\033[0m out of memory (%s)\n", context);
  exit(EXIT_OOM);
}

/// Safe malloc wrapper — aborts on allocation failure.
static void *xmalloc(size_t size) {
  void *p = malloc(size);
  if (!p && size > 0)
    die_oom("malloc");
  return p;
}

/// Safe calloc wrapper — aborts on allocation failure.
static void *xcalloc(size_t count, size_t size) {
  void *p = calloc(count, size);
  if (!p && count > 0 && size > 0)
    die_oom("calloc");
  return p;
}

/// Safe realloc wrapper — preserves original pointer on failure.
static void *xrealloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size);
  if (!p && size > 0)
    die_oom("realloc");
  return p;
}

// ════════════════════════════════════════════════════════════════════
//  Signal handlers (async-signal-safe ONLY)
// ════════════════════════════════════════════════════════════════════

static void handle_sigwinch(int sig) {
  (void)sig;
  g_resized = 1;
}

static void handle_sigint(int sig) {
  (void)sig;
  g_quit = 1;
}

// ════════════════════════════════════════════════════════════════════
//  Terminal cleanup (called from main, not from signal handler)
// ════════════════════════════════════════════════════════════════════

static void cleanup_terminal(void) {
  // Show cursor, reset attributes
  const char restore[] = "\033[?25h\033[0m\n";
  (void)write(STDOUT_FILENO, restore, sizeof(restore) - 1);
}

static void cleanup_resources(void) {
  free(g_frame_buf);
  g_frame_buf = NULL;
  free(g_fb);
  g_fb = NULL;
  free(g_fbval);
  g_fbval = NULL;
  free(g_waves);
  g_waves = NULL;
  free(g_phase);
  g_phase = NULL;
}

// ════════════════════════════════════════════════════════════════════
//  256-color palette functions
// ════════════════════════════════════════════════════════════════════

static inline int clamp6(int v) { return v < 0 ? 0 : (v > 5 ? 5 : v); }

/// Map r,g,b [0-5] to 256-color cube index.
static inline int cube(int r, int g, int b) {
  return 16 + 36 * clamp6(r) + 6 * clamp6(g) + clamp6(b);
}

static int pal_rainbow(double t) {
  int r = (int)(2.5 + 2.5 * sin(TWO_PI * t));
  int g = (int)(2.5 + 2.5 * sin(TWO_PI * t + 2.094));
  int b = (int)(2.5 + 2.5 * sin(TWO_PI * t + 4.189));
  return cube(r, g, b);
}

static int pal_dracula(double t) {
  int r = (int)(2.0 + 3.0 * sin(TWO_PI * t + 0.5));
  int g = (int)(1.0 + 2.0 * sin(TWO_PI * t + 3.5));
  int b = (int)(3.0 + 2.0 * sin(TWO_PI * t + 1.2));
  return cube(r, g, b);
}

static int pal_ocean(double t) {
  int r = (int)(0.5 + 1.5 * sin(TWO_PI * t + 4.0));
  int g = (int)(2.0 + 2.5 * sin(TWO_PI * t + 1.0));
  int b = (int)(3.5 + 1.5 * sin(TWO_PI * t));
  return cube(r, g, b);
}

static int pal_fire(double t) {
  int r = (int)(3.5 + 1.5 * sin(TWO_PI * t));
  int g = (int)(1.5 + 2.0 * sin(TWO_PI * t + 0.8));
  int b = (int)(0.5 + 0.5 * sin(TWO_PI * t + 1.6));
  return cube(r, g, b);
}

static int pal_pastel(double t) {
  int r = (int)(3.5 + 1.5 * sin(TWO_PI * t));
  int g = (int)(3.0 + 1.5 * sin(TWO_PI * t + 2.094));
  int b = (int)(3.5 + 1.5 * sin(TWO_PI * t + 4.189));
  return cube(r, g, b);
}

static int pal_neon(double t) {
  int r = (int)(2.5 + 2.5 * sin(TWO_PI * t));
  int g = (int)(1.0 + 4.0 * sin(TWO_PI * t + 2.5));
  int b = (int)(2.0 + 3.0 * sin(TWO_PI * t + 4.8));
  return cube(r, g, b);
}

static int pal_aurora(double t) {
  int r = (int)(1.0 + 2.0 * sin(TWO_PI * t + 3.8));
  int g = (int)(3.0 + 2.0 * sin(TWO_PI * t));
  int b = (int)(2.0 + 2.5 * sin(TWO_PI * t + 1.8));
  return cube(r, g, b);
}

static int pal_matrix(double t) {
  int g = (int)(1.5 + 3.5 * sin(TWO_PI * t));
  return cube(0, g, 0);
}

static const Palette palettes[] = {
    {"rainbow", pal_rainbow}, {"dracula", pal_dracula}, {"ocean", pal_ocean},
    {"fire", pal_fire},       {"pastel", pal_pastel},   {"neon", pal_neon},
    {"aurora", pal_aurora},   {"matrix", pal_matrix},
};
static const int NUM_PALETTES = (int)(sizeof(palettes) / sizeof(palettes[0]));

static palette_fn find_palette(const char *name) {
  for (int i = 0; i < NUM_PALETTES; i++) {
    if (strcasecmp(palettes[i].name, name) == 0)
      return palettes[i].fn;
  }
  return NULL;
}

// ════════════════════════════════════════════════════════════════════
//  Wave generation helpers
// ════════════════════════════════════════════════════════════════════

static const char *default_glyphs[] = {"█", "▓", "░", "●", "◆",
                                       "╳", "◈", "▪", "⬡", "✦"};
static const int NUM_DEFAULT_GLYPHS = 10;

static void generate_waves(Wave *waves, int n, const char *glyph_override) {
  for (int i = 0; i < n; i++) {
    double t = (double)i / (n > 1 ? (n - 1) : 1);
    waves[i].freq = 0.06 + 0.10 * t;
    waves[i].amp = 0.85 - 0.50 * t;
    waves[i].phase_spd = 0.030 + 0.055 * t;
    waves[i].glyph = glyph_override ? glyph_override
                                    : default_glyphs[i % NUM_DEFAULT_GLYPHS];
  }
}

// ════════════════════════════════════════════════════════════════════
//  Terminal helpers
// ════════════════════════════════════════════════════════════════════

static void term_size(int *rows, int *cols) {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 &&
      w.ws_col > 0) {
    *rows = w.ws_row;
    *cols = w.ws_col;
  } else {
    *rows = 24;
    *cols = 80;
  }
}

// ════════════════════════════════════════════════════════════════════
//  Help / Usage — Premium ASCII Art Banner
// ════════════════════════════════════════════════════════════════════

static void print_version(void) { printf("wave %s\n", WAVE_VERSION); }

/// Compute display width of a UTF-8 string (handles 2-col emoji).
static int display_width(const char *s) {
  int w = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    if (*p < 0x80) {
      w++;
      p++;
    } else if ((*p & 0xF0) == 0xF0) {
      // 4-byte UTF-8 = likely emoji or CJK → 2 columns
      w += 2;
      p += 4;
    } else if ((*p & 0xE0) == 0xE0) {
      w++;
      p += 3;
    } else {
      w++;
      p += 2;
    }
  }
  return w;
}

#define BOX_INNER_W 43

/// Print one box line: │ <content padded to BOX_INNER_W> │
static void print_box_line(int border_color, int content_color,
                           const char *content) {
  int cw = display_width(content);
  int pad = BOX_INNER_W - cw;
  if (pad < 0)
    pad = 0;
  printf("\033[38;5;%dm  │\033[0m", border_color);
  if (content_color > 0)
    printf("\033[1;38;5;%dm%s\033[0m", content_color, content);
  else
    printf("%s", content);
  printf("%*s", pad, "");
  printf("\033[38;5;%dm│\033[0m\n", border_color);
}

static void print_help(void) {
  // ── Banner ────────────────────────────────────────────────────
  printf("\n");
  printf("\033[38;5;39m  ┌");
  for (int i = 0; i < BOX_INNER_W; i++)
    printf("─");
  printf("┐\033[0m\n");

  print_box_line(39, 39, "  ██╗    ██╗ █████╗ ██╗   ██╗███████╗");
  print_box_line(75, 75, "  ██║    ██║██╔══██╗██║   ██║██╔════╝");
  print_box_line(111, 111, "  ██║ █╗ ██║███████║██║   ██║█████╗");
  print_box_line(147, 147, "  ██║███╗██║██╔══██║╚██╗ ██╔╝██╔══╝");
  print_box_line(183, 183, "  ╚███╔███╔╝██║  ██║ ╚████╔╝ ███████╗");
  print_box_line(212, 212, "   ╚══╝╚══╝ ╚═╝  ╚═╝  ╚═══╝  ╚══════╝");
  print_box_line(212, 0, "");

  // Subtitle line (uses dim + 🌊 emoji = 2 display cols)
  {
    char subtitle[128];
    snprintf(subtitle, sizeof(subtitle), "  🌊 Terminal wave visualizer · v%s",
             WAVE_VERSION);
    int cw = display_width(subtitle);
    int pad = BOX_INNER_W - cw;
    if (pad < 0)
      pad = 0;
    printf("\033[38;5;141m  │\033[0m");
    printf("  \033[2;38;5;248m🌊 Terminal wave visualizer · v%s\033[0m",
           WAVE_VERSION);
    printf("%*s", pad, "");
    printf("\033[38;5;141m│\033[0m\n");
  }

  printf("\033[38;5;141m  └");
  for (int i = 0; i < BOX_INNER_W; i++)
    printf("─");
  printf("┘\033[0m\n\n");

  printf("\033[1mUSAGE\033[0m\n"
         "  \033[38;5;248m$\033[0m wave \033[38;5;114m[OPTIONS]\033[0m\n\n"

         "\033[1mOPTIONS\033[0m\n"
         "  \033[38;5;114m-s, --speed\033[0m \033[38;5;248m<float>\033[0m   "
         "Speed multiplier          "
         "\033[2m[default: %.1f]\033[0m\n"
         "  \033[38;5;114m-f, --fps\033[0m   \033[38;5;248m<int>\033[0m     "
         "Target frames per second  "
         "\033[2m[default: %d]\033[0m\n"
         "  \033[38;5;114m-c, --color\033[0m \033[38;5;248m<name>\033[0m    "
         "Color palette             "
         "\033[2m[default: %s]\033[0m\n"
         "  \033[38;5;114m-g, --char\033[0m  \033[38;5;248m<str>\033[0m     "
         "Wave glyph character      "
         "\033[2m[default: auto]\033[0m\n"
         "  \033[38;5;114m-n, --waves\033[0m \033[38;5;248m<int>\033[0m     "
         "Number of waves           "
         "\033[2m[default: %d]\033[0m\n"
         "  \033[38;5;114m-v, --version\033[0m         "
         "Print version\n"
         "  \033[38;5;114m-h, --help\033[0m            "
         "Show this help\n\n",
         DEFAULT_SPEED, DEFAULT_FPS, DEFAULT_PALETTE, DEFAULT_NUM_WAVES);

  // Palette showcase with color previews
  printf("\033[1mPALETTES\033[0m\n");
  for (int i = 0; i < NUM_PALETTES; i++) {
    printf("  ");
    // print 8 colored blocks as a mini gradient preview
    for (int s = 0; s < 8; s++) {
      double t = (double)s / 7.0;
      int c = palettes[i].fn(t);
      printf("\033[38;5;%dm▄\033[0m", c);
    }
    printf("  %-8s", palettes[i].name);
    if ((i % 2) == 1 || i == NUM_PALETTES - 1)
      putchar('\n');
  }

  printf("\n"
         "\033[2m  ╶─ Press Ctrl+C to quit. Resize your terminal to reshape "
         "the waves. ─╴\033[0m\n\n");
}

// ════════════════════════════════════════════════════════════════════
//  Safe number parsing
// ════════════════════════════════════════════════════════════════════

/// Parse a double from a string. Returns false on failure.
static bool parse_double(const char *str, double *out) {
  char *end = NULL;
  errno = 0;
  double val = strtod(str, &end);
  if (errno != 0 || end == str || *end != '\0')
    return false;
  *out = val;
  return true;
}

/// Parse a long from a string. Returns false on failure.
static bool parse_long(const char *str, long *out) {
  char *end = NULL;
  errno = 0;
  long val = strtol(str, &end, 10);
  if (errno != 0 || end == str || *end != '\0')
    return false;
  *out = val;
  return true;
}

// ════════════════════════════════════════════════════════════════════
//  CLI parsing
// ════════════════════════════════════════════════════════════════════

static WaveConfig parse_args(int argc, char **argv) {
  WaveConfig cfg = {
      .speed_mult = DEFAULT_SPEED,
      .fps = DEFAULT_FPS,
      .num_waves = DEFAULT_NUM_WAVES,
      .color_name = DEFAULT_PALETTE,
      .glyph = NULL,
  };

  static struct option long_opts[] = {
      {"speed", required_argument, NULL, 's'},
      {"fps", required_argument, NULL, 'f'},
      {"color", required_argument, NULL, 'c'},
      {"char", required_argument, NULL, 'g'},
      {"waves", required_argument, NULL, 'n'},
      {"version", no_argument, NULL, 'v'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "s:f:c:g:n:vh", long_opts, NULL)) !=
         -1) {
    switch (opt) {
    case 's': {
      double val;
      if (!parse_double(optarg, &val) || val <= 0.0)
        die("invalid speed '%s' (must be a positive number)", optarg);
      cfg.speed_mult = val;
      break;
    }
    case 'f': {
      long val;
      if (!parse_long(optarg, &val))
        die("invalid fps '%s' (must be an integer)", optarg);
      if (val < MIN_FPS || val > MAX_FPS)
        die("fps must be between %d and %d", MIN_FPS, MAX_FPS);
      cfg.fps = (int)val;
      break;
    }
    case 'c':
      if (!find_palette(optarg)) {
        fprintf(stderr,
                "\033[1;31merror:\033[0m unknown palette '%s'\n"
                "available: ",
                optarg);
        for (int i = 0; i < NUM_PALETTES; i++)
          fprintf(stderr, "%s%s", palettes[i].name,
                  i < NUM_PALETTES - 1 ? ", " : "\n");
        exit(EXIT_ERR);
      }
      cfg.color_name = optarg;
      break;
    case 'g':
      cfg.glyph = optarg;
      break;
    case 'n': {
      long val;
      if (!parse_long(optarg, &val))
        die("invalid wave count '%s' (must be an integer)", optarg);
      if (val < MIN_WAVES || val > MAX_WAVES)
        die("wave count must be between %d and %d", MIN_WAVES, MAX_WAVES);
      cfg.num_waves = (int)val;
      break;
    }
    case 'v':
      print_version();
      exit(EXIT_OK);
    case 'h':
      print_help();
      exit(EXIT_OK);
    default:
      print_help();
      exit(EXIT_ERR);
    }
  }
  return cfg;
}

// ════════════════════════════════════════════════════════════════════
//  Main
// ════════════════════════════════════════════════════════════════════

int main(int argc, char **argv) {
  WaveConfig cfg = parse_args(argc, argv);
  palette_fn colorize = find_palette(cfg.color_name);
  if (!colorize) {
    die("internal error: palette '%s' not found", cfg.color_name);
  }

  const int frame_delay = 1000000 / cfg.fps;

  // ── Set up signal handlers ─────────────────────────────────────
  struct sigaction sa_winch;
  memset(&sa_winch, 0, sizeof(sa_winch));
  sa_winch.sa_handler = handle_sigwinch;
  sa_winch.sa_flags = SA_RESTART;
  sigemptyset(&sa_winch.sa_mask);
  sigaction(SIGWINCH, &sa_winch, NULL);

  struct sigaction sa_int;
  memset(&sa_int, 0, sizeof(sa_int));
  sa_int.sa_handler = handle_sigint;
  sigemptyset(&sa_int.sa_mask);
  sigaction(SIGINT, &sa_int, NULL);
  sigaction(SIGTERM, &sa_int, NULL);

  // ── Allocate waves ─────────────────────────────────────────────
  g_waves = xmalloc((size_t)cfg.num_waves * sizeof(Wave));
  g_phase = xcalloc((size_t)cfg.num_waves, sizeof(double));
  generate_waves(g_waves, cfg.num_waves, cfg.glyph);

  // ── Initial terminal state ─────────────────────────────────────
  int rows = 0, cols = 0;
  term_size(&rows, &cols);

  size_t cells = (size_t)rows * (size_t)cols;
  g_fb = xmalloc(cells * sizeof(int));
  g_fbval = xmalloc(cells * sizeof(double));

  size_t buf_cap = cells * MAX_BYTES_PER_CELL + FRAME_BUF_PADDING;
  g_frame_buf = xmalloc(buf_cap);

  // Hide cursor, clear screen
  {
    const char init[] = "\033[?25l\033[2J";
    (void)write(STDOUT_FILENO, init, sizeof(init) - 1);
  }

  unsigned int rng_state = 12345u;
  int frame = 0;

  while (!g_quit) {
    // ── Handle resize ──────────────────────────────────────────
    if (g_resized) {
      g_resized = 0;
      term_size(&rows, &cols);
      cells = (size_t)rows * (size_t)cols;
      buf_cap = cells * MAX_BYTES_PER_CELL + FRAME_BUF_PADDING;

      g_fb = xrealloc(g_fb, cells * sizeof(int));
      g_fbval = xrealloc(g_fbval, cells * sizeof(double));
      g_frame_buf = xrealloc(g_frame_buf, buf_cap);

      // Clear screen on resize to avoid visual artifacts
      const char cls[] = "\033[2J";
      (void)write(STDOUT_FILENO, cls, sizeof(cls) - 1);
    }

    // ── Clear cell buffer ──────────────────────────────────────
    memset(g_fb, 0xFF, cells * sizeof(int)); // -1 fill

    const int mid_y = rows / 2;

    // ── Plot waves ─────────────────────────────────────────────
    for (int w = 0; w < cfg.num_waves; w++) {
      for (int x = 0; x < cols; x++) {
        double y_raw =
            g_waves[w].amp * mid_y * sin(g_waves[w].freq * x + g_phase[w]);
        int y = mid_y + (int)y_raw;
        if (y >= 0 && y < rows) {
          size_t idx = (size_t)y * (size_t)cols + (size_t)x;
          g_fb[idx] = w;
          g_fbval[idx] = (double)x / cols + (double)frame / FRAME_COLOR_DIVISOR;
        }
      }
      g_phase[w] += g_waves[w].phase_spd * cfg.speed_mult;
    }

    // ── Render into frame buffer ───────────────────────────────
    size_t pos = 0;

    // Cursor home
    memcpy(g_frame_buf + pos, "\033[H", 3);
    pos += 3;

    for (int r = 0; r < rows; r++) {
      for (int c = 0; c < cols; c++) {
        // Safety: ensure we never overflow the buffer
        if (pos + MAX_BYTES_PER_CELL >= buf_cap)
          goto flush;

        size_t idx = (size_t)r * (size_t)cols + (size_t)c;
        if (g_fb[idx] >= 0) {
          int w = g_fb[idx];
          double t = fmod(g_fbval[idx] + w * WAVE_COLOR_OFFSET, 1.0);
          if (t < 0.0)
            t += 1.0;
          int color = colorize(t);

          // Write fg color escape via snprintf for safety
          int written = snprintf(g_frame_buf + pos, buf_cap - pos,
                                 "\033[38;5;%dm", color);
          if (written > 0)
            pos += (size_t)written;

          // Write glyph
          const char *gl = g_waves[w].glyph;
          size_t gl_len = strlen(gl);
          if (pos + gl_len + 4 < buf_cap) {
            memcpy(g_frame_buf + pos, gl, gl_len);
            pos += gl_len;
          }

          // Reset attributes
          if (pos + 4 < buf_cap) {
            memcpy(g_frame_buf + pos, "\033[0m", 4);
            pos += 4;
          }
        } else {
          // Subtle starfield background — fast xorshift RNG
          rng_state ^= rng_state << 13;
          rng_state ^= rng_state >> 17;
          rng_state ^= rng_state << 5;

          if ((rng_state % STARFIELD_DENSITY) == 0) {
            int gray = STARFIELD_GRAY_BASE +
                       (int)((rng_state >> 8) % STARFIELD_GRAY_RANGE);
            int written = snprintf(g_frame_buf + pos, buf_cap - pos,
                                   "\033[38;5;%dm.\033[0m", gray);
            if (written > 0)
              pos += (size_t)written;
          } else {
            g_frame_buf[pos++] = ' ';
          }
        }
      }
      if (r < rows - 1) {
        g_frame_buf[pos++] = '\n';
      }
    }

  flush:
    // ── Single write for entire frame ──────────────────────────
    (void)write(STDOUT_FILENO, g_frame_buf, pos);

    frame++;
    usleep((unsigned)frame_delay);
  }

  // ── Graceful cleanup after signal ──────────────────────────────
  cleanup_terminal();
  cleanup_resources();
  return EXIT_OK;
}
