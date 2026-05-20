#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#ifdef __APPLE__
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/socket.h>
#else
#include <cctype>
#include <fstream>
#include <sstream>
#endif

#define WIDTH      1180
#define HEIGHT     720
#define HEADER_H   56
#define SIDEBAR_W  240
#define PAD        14
#define ROW_H      56
#define SAMPLE_MS  500
#define HISTORY    240

static const SDL_Color C_BG        = { 18,  20,  28, 255};
static const SDL_Color C_PANEL     = { 28,  32,  44, 255};
static const SDL_Color C_BORDER    = { 44,  50,  66, 255};
static const SDL_Color C_TEXT      = {220, 224, 232, 255};
static const SDL_Color C_MUTED     = {136, 144, 160, 255};
static const SDL_Color C_DIM       = { 88,  96, 112, 255};
static const SDL_Color C_ACCENT    = {100, 180, 255, 255};
static const SDL_Color C_ACCENT2   = {255, 140, 100, 255};
static const SDL_Color C_SUCCESS   = {120, 200, 140, 255};
static const SDL_Color C_WARNING   = {230, 180,  80, 255};
static const SDL_Color C_DANGER    = {230, 100, 100, 255};
static const SDL_Color C_GRID      = { 38,  42,  56, 255};
static const SDL_Color C_HIGHLIGHT = { 54,  64,  86, 255};
static const SDL_Color C_SIDEBAR   = { 22,  25,  34, 255};
static const SDL_Color C_BTN       = { 44,  50,  66, 255};
static const SDL_Color C_BTN_HOVER = { 60,  68,  90, 255};
static const SDL_Color C_BTN_ACTIVE= { 80,  92, 122, 255};

typedef struct Sample {
  double rx_bps;
  double tx_bps;
} Sample;

typedef struct Interface {
  std::string name;
  uint64_t rx_bytes, tx_bytes;
  uint64_t rx_packets, tx_packets;
  uint64_t rx_errors, tx_errors;
  uint64_t rx_drops, tx_drops;
  uint64_t baseline_rx, baseline_tx;
  Sample current;
  double peak_rx, peak_tx;
  std::deque<Sample> history;
} Interface;

typedef struct Button {
  SDL_Rect bounds;
  std::string label;
  bool hovered;
  bool pressed;
  bool toggled;
} Button;

typedef struct AppData {
  TTF_Font *font_lg, *font_md, *font_sm, *font_xs;
  SDL_Texture *tex_wifi, *tex_arrow_green, *tex_arrow_red;
  std::vector<Interface> interfaces;
  int selected_idx;
  int scroll_offset;
  int sort_mode;        // 0 = name, 1 = activity
  bool paused;
  uint32_t last_sample_ms;
  uint32_t session_start_ms;
  double smoothed_max;
  Button btn_pause, btn_reset, btn_sort;
} AppData;

// Font candidates: first loadable wins.
static const char *FONT_PATHS[] = {
#ifdef __APPLE__
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Verdana.ttf",
    "/Library/Fonts/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
#endif
    NULL,
};

TTF_Font *load_font(int size) {
  for (const char **p = FONT_PATHS; *p; p++) {
    TTF_Font *f = TTF_OpenFont(*p, size);
    if (f) return f;
  }
  std::fprintf(stderr, "[Font] no font found for size %d\n", size);
  return NULL;
}

// Recolors a monochrome icon to `tint`.  Visibility comes from how dark and
// how opaque each source pixel is, so this works whether the icon sits on a
// white background or a transparent one.
SDL_Texture *load_icon_tinted(SDL_Renderer *r, const char *path, SDL_Color tint) {
  SDL_Surface *raw = IMG_Load(path);
  if (!raw) {
    std::fprintf(stderr, "[IMG] failed to load %s: %s\n", path, IMG_GetError());
    return NULL;
  }
  SDL_Surface *surf = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(raw);
  if (!surf) return NULL;
  SDL_LockSurface(surf);
  Uint32 *px = (Uint32 *)surf->pixels;
  int count = (surf->pitch / 4) * surf->h;
  for (int i = 0; i < count; i++) {
    Uint8 cr, cg, cb, ca;
    SDL_GetRGBA(px[i], surf->format, &cr, &cg, &cb, &ca);
    int brightness = (cr + cg + cb) / 3;
    // Combine darkness with the source alpha: transparent or white pixels
    // fade out, dark opaque pixels stay fully tinted.
    Uint8 alpha = (Uint8)(ca * (255 - brightness) / 255);
    px[i] = SDL_MapRGBA(surf->format, tint.r, tint.g, tint.b, alpha);
  }
  SDL_UnlockSurface(surf);
  SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
  SDL_FreeSurface(surf);
  if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  return tex;
}

void load_images(SDL_Renderer *r, AppData *d) {
  d->tex_wifi        = load_icon_tinted(r, "img/wifi.png",  C_ACCENT);
  d->tex_arrow_green = load_icon_tinted(r, "img/arrow.png", C_SUCCESS);
  d->tex_arrow_red   = load_icon_tinted(r, "img/arrow.png", C_DANGER);
}

std::string fmt_bytes(uint64_t b) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double v = (double)b;
  int u = 0;
  while (v >= 1000.0 && u < 5) { v /= 1000.0; u++; }
  char buf[32];
  if (u == 0) std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
  else        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
  return buf;
}

std::string fmt_rate(double bps) {
  const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
  if (bps < 0) bps = 0;
  double v = bps;
  int u = 0;
  while (v >= 1000.0 && u < 4) { v /= 1000.0; u++; }
  char buf[32];
  if (u == 0) std::snprintf(buf, sizeof(buf), "%.0f B/s", v);
  else        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
  return buf;
}

std::string fmt_number(uint64_t v) {
  std::string s = std::to_string(v);
  int n = (int)s.size();
  int first = (n % 3 == 0) ? 3 : (n % 3);
  std::string out;
  for (int i = 0; i < n; i++) {
    if (i > 0 && (i - first) % 3 == 0) out += ',';
    out += s[i];
  }
  return out;
}

std::string fmt_duration(uint32_t sec) {
  int h = sec / 3600;
  int m = (sec / 60) % 60;
  int s = sec % 60;
  char buf[32];
  if (h > 0)      std::snprintf(buf, sizeof(buf), "%dh %dm %ds", h, m, s);
  else if (m > 0) std::snprintf(buf, sizeof(buf), "%dm %ds", m, s);
  else            std::snprintf(buf, sizeof(buf), "%ds", s);
  return buf;
}

// Read raw counters from the OS.
void snapshot(std::vector<Interface> &out) {
  out.clear();
#ifdef __APPLE__
  struct ifaddrs *head = NULL;
  if (getifaddrs(&head) != 0 || !head) return;
  for (struct ifaddrs *ifa = head; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
    struct if_data *d = (struct if_data *)ifa->ifa_data;
    if (!d) continue;
    Interface i = {};
    i.name = ifa->ifa_name ? ifa->ifa_name : "?";
    i.rx_bytes = d->ifi_ibytes;
    i.tx_bytes = d->ifi_obytes;
    i.rx_packets = d->ifi_ipackets;
    i.tx_packets = d->ifi_opackets;
    i.rx_errors = d->ifi_ierrors;
    i.tx_errors = d->ifi_oerrors;
    i.rx_drops = d->ifi_iqdrops;
    out.push_back(i);
  }
  freeifaddrs(head);
#else
  std::ifstream in("/proc/net/dev");
  if (!in) return;
  std::string line;
  std::getline(in, line);
  std::getline(in, line);
  while (std::getline(in, line)) {
    size_t col = line.find(':');
    if (col == std::string::npos) continue;
    std::string name = line.substr(0, col);
    while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(name.begin());
    while (!name.empty() && std::isspace((unsigned char)name.back()))  name.pop_back();
    std::istringstream iss(line.substr(col + 1));
    uint64_t rxb, rxp, rxe, rxd, a, b, c, dd, txb, txp, txe, txd;
    iss >> rxb >> rxp >> rxe >> rxd >> a >> b >> c >> dd
        >> txb >> txp >> txe >> txd;
    if (!iss) continue;
    Interface i = {};
    i.name = name;
    i.rx_bytes = rxb; i.tx_bytes = txb;
    i.rx_packets = rxp; i.tx_packets = txp;
    i.rx_errors = rxe; i.tx_errors = txe;
    i.rx_drops = rxd; i.tx_drops = txd;
    out.push_back(i);
  }
#endif
}

// Take one snapshot, compute rates vs. previous, push into history.
void sample(AppData *d) {
  std::vector<Interface> snap;
  snapshot(snap);
  uint32_t now = SDL_GetTicks();
  double dt = (now - d->last_sample_ms) / 1000.0;

  for (auto &cur : snap) {
    Interface *existing = NULL;
    for (auto &it : d->interfaces) {
      if (it.name == cur.name) { existing = &it; break; }
    }
    if (!existing) {
      cur.baseline_rx = cur.rx_bytes;
      cur.baseline_tx = cur.tx_bytes;
      d->interfaces.push_back(cur);
      continue;
    }
    Sample s = {0, 0};
    if (dt > 0.0) {
      uint64_t drx = (cur.rx_bytes >= existing->rx_bytes)
                         ? cur.rx_bytes - existing->rx_bytes : 0;
      uint64_t dtx = (cur.tx_bytes >= existing->tx_bytes)
                         ? cur.tx_bytes - existing->tx_bytes : 0;
      s.rx_bps = (double)drx / dt;
      s.tx_bps = (double)dtx / dt;
    }
    existing->rx_bytes   = cur.rx_bytes;
    existing->tx_bytes   = cur.tx_bytes;
    existing->rx_packets = cur.rx_packets;
    existing->tx_packets = cur.tx_packets;
    existing->rx_errors  = cur.rx_errors;
    existing->tx_errors  = cur.tx_errors;
    existing->rx_drops   = cur.rx_drops;
    existing->tx_drops   = cur.tx_drops;
    existing->current = s;
    existing->history.push_back(s);
    while (existing->history.size() > HISTORY) existing->history.pop_front();
    if (s.rx_bps > existing->peak_rx) existing->peak_rx = s.rx_bps;
    if (s.tx_bps > existing->peak_tx) existing->peak_tx = s.tx_bps;
  }
}

bool point_in(int x, int y, SDL_Rect r) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void set_color(SDL_Renderer *r, SDL_Color c) {
  SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

void fill_rect(SDL_Renderer *r, SDL_Rect rect, SDL_Color c) {
  set_color(r, c);
  SDL_RenderFillRect(r, &rect);
}

void draw_rect(SDL_Renderer *r, SDL_Rect rect, SDL_Color c) {
  set_color(r, c);
  SDL_RenderDrawRect(r, &rect);
}

void fill_rounded(SDL_Renderer *r, SDL_Rect rect, int radius, SDL_Color c) {
  int rad = std::min(radius, std::min(rect.w, rect.h) / 2);
  if (rad <= 0) { fill_rect(r, rect, c); return; }
  set_color(r, c);
  SDL_Rect mid = {rect.x,       rect.y + rad,            rect.w,           rect.h - 2 * rad};
  SDL_Rect top = {rect.x + rad, rect.y,                  rect.w - 2 * rad, rad};
  SDL_Rect bot = {rect.x + rad, rect.y + rect.h - rad,   rect.w - 2 * rad, rad};
  SDL_RenderFillRect(r, &mid);
  SDL_RenderFillRect(r, &top);
  SDL_RenderFillRect(r, &bot);
  auto corner = [&](int cx, int cy, int sx, int sy) {
    for (int dy = 0; dy <= rad; dy++) {
      int dx = (int)std::round(std::sqrt((double)rad * rad - (double)dy * dy));
      int y = cy + sy * dy;
      int x1 = cx, x2 = cx + sx * dx;
      if (x1 > x2) std::swap(x1, x2);
      SDL_RenderDrawLine(r, x1, y, x2, y);
    }
  };
  corner(rect.x + rad,             rect.y + rad,             -1, -1);
  corner(rect.x + rect.w - rad - 1, rect.y + rad,             +1, -1);
  corner(rect.x + rad,             rect.y + rect.h - rad - 1, -1, +1);
  corner(rect.x + rect.w - rad - 1, rect.y + rect.h - rad - 1, +1, +1);
}

SDL_Point measure(TTF_Font *f, const std::string &s) {
  int w = 0, h = 0;
  if (f) TTF_SizeUTF8(f, s.c_str(), &w, &h);
  return {w, h};
}

void draw_text(SDL_Renderer *r, TTF_Font *f, const std::string &s,
               int x, int y, SDL_Color c) {
  if (!f || s.empty()) return;
  SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
  if (!surf) return;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
  if (tex) {
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
  }
  SDL_FreeSurface(surf);
}

void draw_text_centered(SDL_Renderer *r, TTF_Font *f, const std::string &s,
                        SDL_Rect rect, SDL_Color c) {
  SDL_Point m = measure(f, s);
  draw_text(r, f, s, rect.x + (rect.w - m.x) / 2, rect.y + (rect.h - m.y) / 2, c);
}

void draw_text_right(SDL_Renderer *r, TTF_Font *f, const std::string &s,
                     SDL_Rect rect, SDL_Color c) {
  SDL_Point m = measure(f, s);
  draw_text(r, f, s, rect.x + rect.w - m.x, rect.y + (rect.h - m.y) / 2, c);
}

bool button_handle(Button &b, const SDL_Event &e) {
  if (e.type == SDL_MOUSEMOTION) {
    b.hovered = point_in(e.motion.x, e.motion.y, b.bounds);
  } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
    if (point_in(e.button.x, e.button.y, b.bounds)) b.pressed = true;
  } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
    bool clicked = b.pressed && point_in(e.button.x, e.button.y, b.bounds);
    b.pressed = false;
    return clicked;
  }
  return false;
}

void draw_button(SDL_Renderer *r, TTF_Font *f, const Button &b) {
  SDL_Color bg = C_BTN;
  if (b.toggled || b.pressed) bg = C_BTN_ACTIVE;
  else if (b.hovered)         bg = C_BTN_HOVER;
  fill_rounded(r, b.bounds, 6, bg);
  draw_text_centered(r, f, b.label, b.bounds, C_TEXT);
}

// Returns indices into d->interfaces ordered for display.
std::vector<int> sorted_indices(const AppData *d) {
  std::vector<int> idx;
  for (int i = 0; i < (int)d->interfaces.size(); i++) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [d](int a, int b) {
    const Interface &A = d->interfaces[a];
    const Interface &B = d->interfaces[b];
    if (d->sort_mode == 1) {
      double aa = A.current.rx_bps + A.current.tx_bps;
      double bb = B.current.rx_bps + B.current.tx_bps;
      if (aa != bb) return aa > bb;
    }
    return A.name < B.name;
  });
  return idx;
}

void reset_session(AppData *d) {
  for (auto &it : d->interfaces) {
    it.baseline_rx = it.rx_bytes;
    it.baseline_tx = it.tx_bytes;
    it.peak_rx = it.peak_tx = 0.0;
    it.history.clear();
    it.current = {0, 0};
  }
  d->session_start_ms = SDL_GetTicks();
}

void draw_header(SDL_Renderer *r, AppData *d) {
  SDL_Rect rect = {0, 0, WIDTH, HEADER_H};
  fill_rect(r, rect, C_PANEL);
  set_color(r, C_BORDER);
  SDL_RenderDrawLine(r, 0, HEADER_H - 1, WIDTH, HEADER_H - 1);

  SDL_Rect icon = {14, (HEADER_H - 40) / 2, 40, 40};
  if (d->tex_wifi) {
    SDL_RenderCopy(r, d->tex_wifi, NULL, &icon);
  } else {
    fill_rounded(r, icon, 6, C_ACCENT);
    SDL_Rect inner = {icon.x + 6, icon.y + 6, icon.w - 12, icon.h - 12};
    fill_rounded(r, inner, 3, C_BG);
  }

  draw_text(r, d->font_md, "Network Monitor", 64, 10, C_TEXT);
  std::string sub = "source: ";
#ifdef __APPLE__
  sub += "macOS getifaddrs (AF_LINK / if_data)";
#else
  sub += "/proc/net/dev";
#endif
  if (d->paused) sub += "   \xE2\x80\xA2   PAUSED";
  uint32_t elapsed = (SDL_GetTicks() - d->session_start_ms) / 1000;
  sub += "   \xE2\x80\xA2   session " + fmt_duration(elapsed);
  draw_text(r, d->font_xs, sub, 64, 36, C_MUTED);

  draw_button(r, d->font_sm, d->btn_sort);
  draw_button(r, d->font_sm, d->btn_reset);
  draw_button(r, d->font_sm, d->btn_pause);
}

void draw_sidebar(SDL_Renderer *r, AppData *d, const std::vector<int> &idx) {
  SDL_Rect bounds = {0, HEADER_H, SIDEBAR_W, HEIGHT - HEADER_H};
  fill_rect(r, bounds, C_SIDEBAR);
  set_color(r, C_BORDER);
  SDL_RenderDrawLine(r, bounds.x + bounds.w - 1, bounds.y,
                     bounds.x + bounds.w - 1, bounds.y + bounds.h);

  draw_text(r, d->font_xs, "INTERFACES", bounds.x + 14, bounds.y + 14, C_MUTED);

  SDL_Rect list_area = {bounds.x, bounds.y + 40, bounds.w, bounds.h - 40};
  SDL_RenderSetClipRect(r, &list_area);

  for (int n = 0; n < (int)idx.size(); n++) {
    int i = idx[n];
    const Interface &iface = d->interfaces[i];
    int y = list_area.y + n * ROW_H - d->scroll_offset;
    if (y + ROW_H < list_area.y) continue;
    if (y > list_area.y + list_area.h) break;

    SDL_Rect row = {list_area.x + 6, y + 4, list_area.w - 12, ROW_H - 8};
    SDL_Color bg = C_SIDEBAR;
    if (i == d->selected_idx) bg = C_HIGHLIGHT;
    fill_rounded(r, row, 6, bg);
    if (i == d->selected_idx) {
      SDL_Rect bar = {row.x, row.y + 8, 3, row.h - 16};
      fill_rect(r, bar, C_ACCENT);
    }
    draw_text(r, d->font_sm, iface.name, row.x + 14, row.y + 6, C_TEXT);

    const int ico = 12;
    int sub_y = row.y + 30;
    int icon_y = sub_y + 1;
    std::string rx_str = fmt_rate(iface.current.rx_bps);
    std::string tx_str = fmt_rate(iface.current.tx_bps);

    // RX (download) → red arrow rotated 180° to point DOWN.
    SDL_Rect rx_icon = {row.x + 14, icon_y, ico, ico};
    if (d->tex_arrow_red) {
      SDL_RenderCopyEx(r, d->tex_arrow_red, NULL, &rx_icon, 180, NULL, SDL_FLIP_NONE);
    }
    draw_text(r, d->font_xs, rx_str, rx_icon.x + ico + 4, sub_y, C_MUTED);

    SDL_Point rx_w = measure(d->font_xs, rx_str);
    int tx_x = rx_icon.x + ico + 4 + rx_w.x + 10;
    // TX (upload) → green arrow, no rotation (PNG already points up).
    SDL_Rect tx_icon = {tx_x, icon_y, ico, ico};
    if (d->tex_arrow_green) {
      SDL_RenderCopy(r, d->tex_arrow_green, NULL, &tx_icon);
    }
    draw_text(r, d->font_xs, tx_str, tx_x + ico + 4, sub_y, C_MUTED);
  }

  SDL_RenderSetClipRect(r, NULL);

  int content_h = (int)idx.size() * ROW_H;
  if (content_h > list_area.h) {
    double frac = (double)list_area.h / content_h;
    int thumb_h = std::max(20, (int)(list_area.h * frac));
    int max_scroll = content_h - list_area.h;
    double sf = (max_scroll > 0) ? (double)d->scroll_offset / max_scroll : 0.0;
    int thumb_y = list_area.y + (int)(sf * (list_area.h - thumb_h));
    SDL_Rect thumb = {list_area.x + list_area.w - 6, thumb_y, 4, thumb_h};
    fill_rounded(r, thumb, 2, C_BORDER);
  }
}

void draw_stat_card(SDL_Renderer *r, AppData *d, SDL_Rect rect,
                    const std::string &title, const std::string &value,
                    const std::string &sub, SDL_Color accent,
                    SDL_Texture *icon, double icon_angle) {
  fill_rounded(r, rect, 8, C_PANEL);
  draw_rect(r, rect, C_BORDER);
  SDL_Rect stripe = {rect.x, rect.y + 12, 3, rect.h - 24};
  fill_rect(r, stripe, accent);
  draw_text(r, d->font_xs, title, rect.x + 16, rect.y + 12, C_MUTED);
  draw_text(r, d->font_lg, value, rect.x + 16, rect.y + 30, C_TEXT);
  if (!sub.empty()) draw_text(r, d->font_xs, sub, rect.x + 16, rect.y + rect.h - 22, C_DIM);
  if (icon) {
    const int sz = 32;
    SDL_Rect ir = {rect.x + rect.w - sz - 12, rect.y + (rect.h - sz) / 2, sz, sz};
    SDL_RenderCopyEx(r, icon, NULL, &ir, icon_angle, NULL, SDL_FLIP_NONE);
  }
}

void draw_cards(SDL_Renderer *r, AppData *d, const Interface &iface) {
  const int cx = SIDEBAR_W + PAD;
  const int cy = HEADER_H + PAD;
  const int cw = WIDTH - SIDEBAR_W - 2 * PAD;
  const int ch = HEIGHT - HEADER_H - 2 * PAD;
  const int top_h = 100;
  const int bot_h = 90;
  const int gap = PAD;
  const int col_w = (cw - 3 * gap) / 4;

  SDL_Rect card_dl = {cx,                       cy, col_w, top_h};
  SDL_Rect card_ul = {cx + 1 * (col_w + gap),   cy, col_w, top_h};
  SDL_Rect card_rx = {cx + 2 * (col_w + gap),   cy, col_w, top_h};
  SDL_Rect card_tx = {cx + 3 * (col_w + gap),   cy, col_w, top_h};

  uint64_t rx_total = iface.rx_bytes - iface.baseline_rx;
  uint64_t tx_total = iface.tx_bytes - iface.baseline_tx;

  draw_stat_card(r, d, card_dl, "DOWNLOAD",
                 fmt_rate(iface.current.rx_bps),
                 "peak " + fmt_rate(iface.peak_rx), C_ACCENT,
                 d->tex_arrow_red, 180.0);
  draw_stat_card(r, d, card_ul, "UPLOAD",
                 fmt_rate(iface.current.tx_bps),
                 "peak " + fmt_rate(iface.peak_tx), C_ACCENT2,
                 d->tex_arrow_green, 0.0);
  draw_stat_card(r, d, card_rx, "SESSION RX",
                 fmt_bytes(rx_total),
                 "lifetime " + fmt_bytes(iface.rx_bytes), C_SUCCESS,
                 NULL, 0.0);
  draw_stat_card(r, d, card_tx, "SESSION TX",
                 fmt_bytes(tx_total),
                 "lifetime " + fmt_bytes(iface.tx_bytes), C_WARNING,
                 NULL, 0.0);

  const int bot_y = cy + ch - bot_h;
  const int bot_col_w = (cw - gap) / 2;
  SDL_Rect rect_pkt = {cx,                   bot_y, bot_col_w, bot_h};
  SDL_Rect rect_err = {cx + bot_col_w + gap, bot_y, bot_col_w, bot_h};

  fill_rounded(r, rect_pkt, 8, C_PANEL);
  draw_rect(r, rect_pkt, C_BORDER);
  draw_text(r, d->font_xs, "PACKETS", rect_pkt.x + 16, rect_pkt.y + 12, C_MUTED);
  draw_text(r, d->font_sm, "rx " + fmt_number(iface.rx_packets),
            rect_pkt.x + 16, rect_pkt.y + 32, C_ACCENT);
  draw_text(r, d->font_sm, "tx " + fmt_number(iface.tx_packets),
            rect_pkt.x + 16, rect_pkt.y + 56, C_ACCENT2);

  fill_rounded(r, rect_err, 8, C_PANEL);
  draw_rect(r, rect_err, C_BORDER);
  draw_text(r, d->font_xs, "ERRORS / DROPS", rect_err.x + 16, rect_err.y + 12, C_MUTED);
  SDL_Color err_color = (iface.rx_errors + iface.tx_errors > 0) ? C_DANGER : C_DIM;
  draw_text(r, d->font_sm,
            "err rx " + fmt_number(iface.rx_errors) +
            "  tx "    + fmt_number(iface.tx_errors),
            rect_err.x + 16, rect_err.y + 32, err_color);
  draw_text(r, d->font_sm,
            "drp rx " + fmt_number(iface.rx_drops) +
            "  tx "    + fmt_number(iface.tx_drops),
            rect_err.x + 16, rect_err.y + 56, C_DIM);
}

void draw_graph(SDL_Renderer *r, AppData *d, const Interface &iface) {
  const int cx = SIDEBAR_W + PAD;
  const int cy = HEADER_H + PAD;
  const int cw = WIDTH - SIDEBAR_W - 2 * PAD;
  const int ch = HEIGHT - HEADER_H - 2 * PAD;
  const int top_h = 100;
  const int bot_h = 90;

  SDL_Rect bounds = {cx, cy + top_h + PAD, cw, ch - top_h - bot_h - 2 * PAD};
  fill_rounded(r, bounds, 8, C_PANEL);
  draw_rect(r, bounds, C_BORDER);

  draw_text(r, d->font_sm,
            "Network Throughput - " + iface.name,
            bounds.x + 14, bounds.y + 10, C_TEXT);

  const int pad_t = 32, pad_b = 22, pad_l = 70, pad_r = 14;
  SDL_Rect plot = {bounds.x + pad_l, bounds.y + pad_t,
                   bounds.w - pad_l - pad_r, bounds.h - pad_t - pad_b};

  double peak = 1024.0;
  for (auto &s : iface.history) {
    if (s.rx_bps > peak) peak = s.rx_bps;
    if (s.tx_bps > peak) peak = s.tx_bps;
  }
  d->smoothed_max = d->smoothed_max * 0.85 + peak * 0.15;
  double scale = std::max(d->smoothed_max, 1024.0);

  for (int i = 0; i <= 4; i++) {
    int y = plot.y + plot.h - (i * plot.h / 4);
    set_color(r, C_GRID);
    SDL_RenderDrawLine(r, plot.x, y, plot.x + plot.w, y);
    double val = scale * i / 4.0;
    SDL_Rect lbl = {bounds.x + 4, y - 8, pad_l - 10, 16};
    draw_text_right(r, d->font_xs, fmt_rate(val), lbl, C_MUTED);
  }

  if (iface.history.size() < 2) {
    draw_text_centered(r, d->font_sm, "collecting samples\xE2\x80\xA6", plot, C_DIM);
    return;
  }

  SDL_RenderSetClipRect(r, &plot);
  const int baseline = plot.y + plot.h;
  SDL_Color rx_area = C_ACCENT;  rx_area.a = 56;
  SDL_Color tx_area = C_ACCENT2; tx_area.a = 56;

  auto sx = [&](size_t i) {
    double t = (double)i / (iface.history.size() - 1);
    return plot.x + (int)std::round(t * plot.w);
  };
  auto sy = [&](double v) {
    double t = v / scale;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return plot.y + plot.h - (int)std::round(t * plot.h);
  };

  for (size_t i = 1; i < iface.history.size(); i++) {
    int x0 = sx(i - 1), x1 = sx(i);
    int y_rx0 = sy(iface.history[i - 1].rx_bps);
    int y_rx1 = sy(iface.history[i].rx_bps);
    int y_tx0 = sy(iface.history[i - 1].tx_bps);
    int y_tx1 = sy(iface.history[i].tx_bps);

    set_color(r, rx_area);
    for (int x = x0; x <= x1; x++) {
      double t = (x1 == x0) ? 0.0 : (double)(x - x0) / (x1 - x0);
      int y = (int)std::round(y_rx0 * (1 - t) + y_rx1 * t);
      SDL_RenderDrawLine(r, x, y, x, baseline);
    }
    set_color(r, tx_area);
    for (int x = x0; x <= x1; x++) {
      double t = (x1 == x0) ? 0.0 : (double)(x - x0) / (x1 - x0);
      int y = (int)std::round(y_tx0 * (1 - t) + y_tx1 * t);
      SDL_RenderDrawLine(r, x, y, x, baseline);
    }
    set_color(r, C_ACCENT);
    SDL_RenderDrawLine(r, x0, y_rx0,     x1, y_rx1);
    SDL_RenderDrawLine(r, x0, y_rx0 - 1, x1, y_rx1 - 1);
    set_color(r, C_ACCENT2);
    SDL_RenderDrawLine(r, x0, y_tx0,     x1, y_tx1);
    SDL_RenderDrawLine(r, x0, y_tx0 - 1, x1, y_tx1 - 1);
  }

  SDL_RenderSetClipRect(r, NULL);

  int legend_y = bounds.y + 10;
  int legend_x = bounds.x + bounds.w - 240;
  fill_rect(r, {legend_x,        legend_y + 3, 10, 10}, C_ACCENT);
  draw_text(r, d->font_xs, "Download", legend_x + 16,  legend_y, C_MUTED);
  fill_rect(r, {legend_x + 110,  legend_y + 3, 10, 10}, C_ACCENT2);
  draw_text(r, d->font_xs, "Upload",   legend_x + 126, legend_y, C_MUTED);
}

void initialize(AppData *d) {
  d->font_lg = load_font(22);
  d->font_md = load_font(18);
  d->font_sm = load_font(13);
  d->font_xs = load_font(11);
  d->tex_wifi = NULL;
  d->tex_arrow_green = NULL;
  d->tex_arrow_red = NULL;
  d->selected_idx = -1;
  d->scroll_offset = 0;
  d->sort_mode = 0;
  d->paused = false;
  d->smoothed_max = 1024.0;

  const int btn_h = 28;
  const int btn_y = (HEADER_H - btn_h) / 2;
  const int gap = 8;
  const int pause_w = 84, reset_w = 72, sort_w = 124;
  const int x_right = WIDTH - PAD;
  d->btn_pause.bounds = {x_right - pause_w,                       btn_y, pause_w, btn_h};
  d->btn_pause.label  = "Pause";
  d->btn_reset.bounds = {d->btn_pause.bounds.x - gap - reset_w,   btn_y, reset_w, btn_h};
  d->btn_reset.label  = "Reset";
  d->btn_sort.bounds  = {d->btn_reset.bounds.x - gap - sort_w,    btn_y, sort_w,  btn_h};
  d->btn_sort.label   = "Sort: name";
}

void render(SDL_Renderer *r, AppData *d) {
  fill_rect(r, {0, 0, WIDTH, HEIGHT}, C_BG);
  draw_header(r, d);
  std::vector<int> idx = sorted_indices(d);
  draw_sidebar(r, d, idx);
  if (d->selected_idx >= 0 && d->selected_idx < (int)d->interfaces.size()) {
    const Interface &iface = d->interfaces[d->selected_idx];
    draw_cards(r, d, iface);
    draw_graph(r, d, iface);
  } else {
    SDL_Rect msg = {SIDEBAR_W + PAD, HEADER_H + PAD,
                    WIDTH - SIDEBAR_W - 2 * PAD,
                    HEIGHT - HEADER_H - 2 * PAD};
    draw_text_centered(r, d->font_md, "Waiting for interface data\xE2\x80\xA6", msg, C_DIM);
  }
  SDL_RenderPresent(r);
}

void cleanup(AppData *d) {
  if (d->font_lg) TTF_CloseFont(d->font_lg);
  if (d->font_md) TTF_CloseFont(d->font_md);
  if (d->font_sm) TTF_CloseFont(d->font_sm);
  if (d->font_xs) TTF_CloseFont(d->font_xs);
  if (d->tex_wifi)        SDL_DestroyTexture(d->tex_wifi);
  if (d->tex_arrow_green) SDL_DestroyTexture(d->tex_arrow_green);
  if (d->tex_arrow_red)   SDL_DestroyTexture(d->tex_arrow_red);
}

int main(int, char *[]) {
  SDL_Init(SDL_INIT_VIDEO);
  IMG_Init(IMG_INIT_PNG);
  TTF_Init();

  SDL_Window *window = SDL_CreateWindow("Network Monitor",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  AppData data;
  initialize(&data);
  load_images(renderer, &data);
  data.session_start_ms = SDL_GetTicks();
  data.last_sample_ms = SDL_GetTicks();
  sample(&data);
  if (!data.interfaces.empty()) data.selected_idx = 0;

  bool running = true;
  SDL_Event event;
  while (running) {
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
      else if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
          case SDLK_ESCAPE:
          case SDLK_q:
            running = false;
            break;
          case SDLK_p:
            data.paused = !data.paused;
            data.btn_pause.label   = data.paused ? "Resume" : "Pause";
            data.btn_pause.toggled = data.paused;
            break;
          case SDLK_r:
            reset_session(&data);
            break;
          case SDLK_s:
            data.sort_mode = (data.sort_mode + 1) % 2;
            data.btn_sort.label = data.sort_mode == 0 ? "Sort: name" : "Sort: activity";
            break;
          case SDLK_UP:
          case SDLK_DOWN: {
            std::vector<int> idx = sorted_indices(&data);
            int pos = 0;
            for (int n = 0; n < (int)idx.size(); n++) {
              if (idx[n] == data.selected_idx) { pos = n; break; }
            }
            pos += (event.key.keysym.sym == SDLK_UP ? -1 : 1);
            if (pos < 0) pos = 0;
            if (pos >= (int)idx.size()) pos = (int)idx.size() - 1;
            if (!idx.empty()) data.selected_idx = idx[pos];
            break;
          }
        }
      } else if (event.type == SDL_MOUSEWHEEL) {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        SDL_Rect sb = {0, HEADER_H, SIDEBAR_W, HEIGHT - HEADER_H};
        if (point_in(mx, my, sb)) {
          data.scroll_offset -= event.wheel.y * 30;
          int content_h = (int)data.interfaces.size() * ROW_H;
          int list_h = HEIGHT - HEADER_H - 40;
          int max_scroll = std::max(0, content_h - list_h);
          if (data.scroll_offset < 0) data.scroll_offset = 0;
          if (data.scroll_offset > max_scroll) data.scroll_offset = max_scroll;
        }
      } else if (event.type == SDL_MOUSEBUTTONDOWN &&
                 event.button.button == SDL_BUTTON_LEFT) {
        SDL_Rect list_area = {0, HEADER_H + 40, SIDEBAR_W, HEIGHT - HEADER_H - 40};
        if (point_in(event.button.x, event.button.y, list_area)) {
          int rel_y = event.button.y - list_area.y + data.scroll_offset;
          int row = rel_y / ROW_H;
          std::vector<int> idx = sorted_indices(&data);
          if (row >= 0 && row < (int)idx.size()) {
            data.selected_idx = idx[row];
          }
        }
      }

      if (button_handle(data.btn_pause, event)) {
        data.paused = !data.paused;
        data.btn_pause.label   = data.paused ? "Resume" : "Pause";
        data.btn_pause.toggled = data.paused;
      }
      if (button_handle(data.btn_reset, event)) {
        reset_session(&data);
      }
      if (button_handle(data.btn_sort, event)) {
        data.sort_mode = (data.sort_mode + 1) % 2;
        data.btn_sort.label = data.sort_mode == 0 ? "Sort: name" : "Sort: activity";
      }
    }

    if (!data.paused && SDL_GetTicks() - data.last_sample_ms >= SAMPLE_MS) {
      sample(&data);
      data.last_sample_ms = SDL_GetTicks();
    }

    render(renderer, &data);
    SDL_Delay(16);
  }

  cleanup(&data);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  TTF_Quit();
  IMG_Quit();
  SDL_Quit();

  return 0;
}
