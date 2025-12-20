// gamma.c — DRM GAMMA_LUT setter with presets + --list + reset
//
// Build:
//   gcc -std=c11 -O2 -D_GNU_SOURCE -DDEFAULT_CRTC=68 gamma.c -o gamma $(pkg-config --cflags --libs libdrm) -lm
//
// Usage:
//   ./gamma [--crtc <id>] [--presets <file>] <gamma_pow> [lift gain r g b]
//   ./gamma [--crtc <id>] [--presets <file>] <preset-name>
//   ./gamma [--presets <file>] --list
//
// Presets search order (unless overridden with --presets <file>):
//   1) ./presets.ini
//   2) /etc/gamma-presets.ini
//
// Built-in preset: "reset" → gamma=1, lift=0, gain=1, r=g=b=1

#ifndef DEFAULT_CRTC
#define DEFAULT_CRTC 68
#endif

#define _GNU_SOURCE 1
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_mode.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Some systems don’t expose O_CLOEXEC unless _GNU_SOURCE; add fallback */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* Safe bounds to avoid black/white screens */
#define GAMMA_MIN 0.20
#define GAMMA_MAX 5.00
#define LIFT_MIN  -0.50
#define LIFT_MAX   0.50
#define GAIN_MIN   0.50
#define GAIN_MAX   1.50
#define MULT_MIN   0.50
#define MULT_MAX   1.50

/* ----------------- Helpers ----------------- */

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [--crtc <id>] [--presets <file>] <gamma_pow> [lift gain r g b]\n"
        "  %s [--crtc <id>] [--presets <file>] <preset-name>\n"
        "  %s [--presets <file>] --list\n"
        "Default CRTC: %u\n"
        "Preset search order (unless --presets given):\n"
        "  ./presets.ini\n"
        "  /etc/gamma-presets.ini\n"
        "Ranges:\n"
        "  gamma ∈ [%.2f, %.2f]\n"
        "  lift  ∈ [%.2f, %.2f]\n"
        "  gain  ∈ [%.2f, %.2f]\n"
        "  r,g,b ∈ [%.2f, %.2f]\n",
        argv0, argv0, argv0, DEFAULT_CRTC,
        GAMMA_MIN, GAMMA_MAX,
        LIFT_MIN, LIFT_MAX,
        GAIN_MIN, GAIN_MAX,
        MULT_MIN, MULT_MAX
    );
}

static bool parse_uint32(const char *s, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno || end == s || (end && *end)) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_double_strict(const char *s, double *out) {
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end != '\0') return false;
    if (!isfinite(v)) return false;
    *out = v;
    return true;
}

static bool parse_double_in_range(const char *label, const char *s,
                                  double minv, double maxv, double *out) {
    double v;
    if (!parse_double_strict(s, &v)) {
        fprintf(stderr, "Invalid %s: '%s'\n", label, s);
        return false;
    }
    if (v < minv || v > maxv) {
        fprintf(stderr, "%s out of range: %g (allowed %.2f..%.2f)\n",
                label, v, minv, maxv);
        return false;
    }
    *out = v;
    return true;
}

static inline uint16_t u16clamp(double x) {
    if (x < 0.0) return 0;
    if (x > 65535.0) return 65535;
    return (uint16_t)(x + 0.5);
}

/* -------------- INI handling -------------- */

struct preset_vals {
    bool have_gamma, have_lift, have_gain, have_r, have_g, have_b;
    double gamma, lift, gain, r, g, b;
    bool have_crtc;
    uint32_t crtc;
};

static void s_trim(char *s) {
    /* left trim */
    char *p = s;
    while (*p && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n')) p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    /* right trim */
    size_t n = strlen(s);
    while (n>0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n]='\0';
    /* strip UTF-8 BOM if present */
    if ((unsigned char)s[0]==0xEF && (unsigned char)s[1]==0xBB && (unsigned char)s[2]==0xBF) {
        memmove(s, s+3, strlen(s+3)+1);
    }
}

static bool file_exists(const char *path) {
    return access(path, R_OK) == 0;
}

static int list_presets_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *sc = strpbrk(line, "#;");
        if (sc) *sc = '\0';
        s_trim(line);
        if (!*line) continue;
        if (line[0]=='[') {
            char *rb = strchr(line, ']');
            if (rb) {
                *rb = '\0';
                char name[256];
                /* safe copy of section name */
                size_t len = strnlen(line+1, sizeof(name)-1);
                memcpy(name, line+1, len);
                name[len] = '\0';
                s_trim(name);
                if (name[0] && strcmp(name, "config") != 0) {
                    if (count == 0) printf("Available presets in %s:\n", path);
                    printf("  %s\n", name);
                    count++;
                }
            }
        }
    }
    fclose(f);
    return count;
}

/* return: 1=loaded, 0=not found, -1=parse error */
static int load_preset_from_file(const char *path, const char *want, struct preset_vals *pv) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[512];
    char current[256] = {0};
    bool wanted = false;
    int status = 0; /* 0=notfound, 1=loaded, -1=error */

    while (fgets(line, sizeof(line), f)) {
        char *sc = strpbrk(line, "#;");
        if (sc) *sc = '\0';
        s_trim(line);
        if (!*line) continue;

        if (line[0]=='[') {
            char *rb = strchr(line, ']');
            if (rb) {
                *rb = '\0';
                /* safe copy of section name */
                size_t len = strnlen(line+1, sizeof(current)-1);
                memcpy(current, line+1, len);
                current[len] = '\0';
                s_trim(current);
                wanted = (strcmp(current, want) == 0);
            }
            continue;
        }

        if (!wanted) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        s_trim(key); s_trim(val);

        double dtmp; uint32_t utmp;
        if (strcmp(key, "gamma")==0) {
            if (parse_double_in_range("gamma", val, GAMMA_MIN, GAMMA_MAX, &dtmp)) { pv->gamma=dtmp; pv->have_gamma=true; status=1; }
            else { status=-1; break; }
        } else if (strcmp(key, "lift")==0) {
            if (parse_double_in_range("lift", val, LIFT_MIN, LIFT_MAX, &dtmp)) { pv->lift=dtmp; pv->have_lift=true; status=1; }
            else { status=-1; break; }
        } else if (strcmp(key, "gain")==0) {
            if (parse_double_in_range("gain", val, GAIN_MIN, GAIN_MAX, &dtmp)) { pv->gain=dtmp; pv->have_gain=true; status=1; }
            else { status=-1; break; }
        } else if (strcmp(key, "r")==0) {
            if (parse_double_in_range("r", val, MULT_MIN, MULT_MAX, &dtmp)) { pv->r=dtmp; pv->have_r=true; status=1; }
            else { status=-1; break; }
        } else if (strcmp(key, "g")==0) {
            if (parse_double_in_range("g", val, MULT_MIN, MULT_MAX, &dtmp)) { pv->g=dtmp; pv->have_g=true; status=1; }
            else { status=-1; break; }
        } else if (strcmp(key, "b")==0) {
            if (parse_double_in_range("b", val, MULT_MIN, MULT_MAX, &dtmp)) { pv->b=dtmp; pv->have_b=true; status=1; }
            else { status=-1; break; }
        } else if (strcmp(key, "crtc")==0) {
            if (parse_uint32(val, &utmp)) { pv->crtc=utmp; pv->have_crtc=true; status=1; }
            else { fprintf(stderr,"Invalid crtc in preset: '%s'\n", val); status=-1; break; }
        } else {
            /* ignore unknown keys */
        }
    }

    fclose(f);
    return status;
}

/* return: 1=loaded, 0=not found, -1=parse error */
static int load_config_crtc_from_file(const char *path, uint32_t *out_crtc) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[512];
    bool in_config = false;
    int status = 0;

    while (fgets(line, sizeof(line), f)) {
        char *sc = strpbrk(line, "#;");
        if (sc) *sc = '\0';
        s_trim(line);
        if (!*line) continue;

        if (line[0]=='[') {
            char *rb = strchr(line, ']');
            if (rb) {
                *rb = '\0';
                char name[256];
                size_t len = strnlen(line+1, sizeof(name)-1);
                memcpy(name, line+1, len);
                name[len] = '\0';
                s_trim(name);
                in_config = (strcmp(name, "config") == 0);
            }
            continue;
        }

        if (!in_config) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        s_trim(key); s_trim(val);

        if (strcmp(key, "crtc") == 0) {
            uint32_t utmp;
            if (parse_uint32(val, &utmp)) {
                *out_crtc = utmp;
                status = 1;
            } else {
                fprintf(stderr, "Invalid crtc in config: '%s'\n", val);
                status = -1;
            }
            break;
        }
    }

    fclose(f);
    return status;
}

static int load_config_crtc(const char *preset_path, uint32_t *out_crtc) {
    if (preset_path) {
        return load_config_crtc_from_file(preset_path, out_crtc);
    }

    int st = load_config_crtc_from_file("./presets.ini", out_crtc);
    if (st != 0) return st;
    return load_config_crtc_from_file("/etc/gamma-presets.ini", out_crtc);
}

static int load_preset(const char *name, const char *preset_path, struct preset_vals *pv) {
    memset(pv, 0, sizeof(*pv));

    if (strcmp(name, "reset")==0) {
        pv->have_gamma = pv->have_lift = pv->have_gain = pv->have_r = pv->have_g = pv->have_b = true;
        pv->gamma = 1.0; pv->lift = 0.0; pv->gain = 1.0; pv->r = pv->g = pv->b = 1.0;
        return 1;
    }

    if (preset_path) {
        return load_preset_from_file(preset_path, name, pv);
    }

    int st = load_preset_from_file("./presets.ini", name, pv);
    if (st != 0) return st;
    st = load_preset_from_file("/etc/gamma-presets.ini", name, pv);
    return st; /* 1=ok, 0=not found, -1=error */
}

static void list_all_presets(const char *preset_path) {
    int total = 0;
    if (preset_path) {
        if (file_exists(preset_path)) {
            total += list_presets_from_file(preset_path);
        }
    } else {
        if (file_exists("./presets.ini")) total += list_presets_from_file("./presets.ini");
        if (file_exists("/etc/gamma-presets.ini")) total += list_presets_from_file("/etc/gamma-presets.ini");
    }
    if (total == 0) {
        if (preset_path) {
            printf("No presets found in %s.\n", preset_path);
        } else {
            printf("No presets.ini found.\n");
        }
    }
    printf("  reset\n"); /* built-in */
}

/* --------------- DRM work --------------- */

static int set_gamma_lut(int fd, uint32_t crtc_id,
                  double gamma_pow,
                  double lift,
                  double gain,
                  double r_mult,
                  double g_mult,
                  double b_mult)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!props) {
        perror("drmModeObjectGetProperties");
        return -1;
    }

    uint32_t lut_prop = 0;
    uint64_t lut_size = 256;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        if (!strcmp(p->name, "GAMMA_LUT")) {
            lut_prop = p->prop_id;
        } else if (!strcmp(p->name, "GAMMA_LUT_SIZE")) {
            lut_size = props->prop_values[i];
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);

    if (!lut_prop || lut_size == 0) {
        fprintf(stderr, "CRTC %u has no GAMMA_LUT/GAMMA_LUT_SIZE\n", crtc_id);
        return -1;
    }

    struct drm_color_lut *lut = calloc(lut_size, sizeof(*lut));
    if (!lut) { perror("calloc(lut)"); return -1; }

    for (uint32_t i = 0; i < lut_size; i++) {
        double x = (double)i / (double)(lut_size - 1);
        double y = pow(fmax(0.0, x + lift), gamma_pow) * gain;
        if (y < 0.0) y = 0.0;
        if (y > 1.0) y = 1.0;

        double r = fmax(0.0, fmin(1.0, y * r_mult));
        double g = fmax(0.0, fmin(1.0, y * g_mult));
        double b = fmax(0.0, fmin(1.0, y * b_mult));

        lut[i].red   = u16clamp(r * 65535.0);
        lut[i].green = u16clamp(g * 65535.0);
        lut[i].blue  = u16clamp(b * 65535.0);
    }

    uint32_t blob_id = 0;
    int ret = drmModeCreatePropertyBlob(fd, lut, sizeof(*lut) * lut_size, &blob_id);
    free(lut);
    if (ret) { perror("drmModeCreatePropertyBlob"); return ret; }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) {
        fprintf(stderr, "drmModeAtomicAlloc failed\n");
        drmModeDestroyPropertyBlob(fd, blob_id);
        return -1;
    }

    ret = drmModeAtomicAddProperty(req, crtc_id, lut_prop, blob_id);
    if (ret < 0) {
        fprintf(stderr, "drmModeAtomicAddProperty failed: %d\n", ret);
        drmModeAtomicFree(req);
        drmModeDestroyPropertyBlob(fd, blob_id);
        return ret;
    }

    ret = drmModeAtomicCommit(fd, req, 0, NULL);
    if (ret) perror("drmModeAtomicCommit");

    drmModeAtomicFree(req);
    drmModeDestroyPropertyBlob(fd, blob_id);
    return ret;
}

/* ------------------- main ------------------- */

int main(int argc, char **argv) {
    uint32_t crtc_id = DEFAULT_CRTC;
    bool crtc_override = false;
    const char *preset_path = NULL;
    bool list_mode = false;

    int i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--list")) {
            list_mode = true;
            i++;
        } else if (!strcmp(argv[i], "--crtc")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--crtc requires an argument.\n");
                return 2;
            }
            uint32_t tmp = 0;
            if (!parse_uint32(argv[i+1], &tmp)) {
                fprintf(stderr, "Invalid --crtc value: %s\n", argv[i+1]);
                print_usage(argv[0]); return 2;
            }
            crtc_id = tmp;
            crtc_override = true;
            i += 2;
        } else if (!strcmp(argv[i], "--presets")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--presets requires a filepath argument.\n");
                return 2;
            }
            preset_path = argv[i+1];
            i += 2;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        } else {
            break;
        }
    }

    if (!crtc_override) {
        uint32_t config_crtc = 0;
        int st = load_config_crtc(preset_path, &config_crtc);
        if (st < 0) return 2;
        if (st > 0) crtc_id = config_crtc;
    }

    if (list_mode) {
        if (i != argc) {
            fprintf(stderr, "--list does not take positional arguments.\n");
            return 2;
        }
        list_all_presets(preset_path);
        return 0;
    }

    if (i >= argc) {
        fprintf(stderr, "Missing arguments.\n");
        print_usage(argv[0]); return 2;
    }

    /* Numeric path (<gamma> [lift gain r g b]) or preset-name */
    double gamma_pow;
    bool first_is_number = parse_double_strict(argv[i], &gamma_pow);

    double lift = 0.0, gain = 1.0, r = 1.0, g = 1.0, b = 1.0;

    if (first_is_number) {
        int remaining = argc - i;
        if (remaining < 1 || remaining > 6) {
            fprintf(stderr, "Invalid number of arguments (%d). Expected 1..6 after options.\n", remaining);
            print_usage(argv[0]); return 2;
        }
        if (gamma_pow < GAMMA_MIN || gamma_pow > GAMMA_MAX) {
            fprintf(stderr, "gamma out of range: %g (%.2f..%.2f)\n", gamma_pow, GAMMA_MIN, GAMMA_MAX);
            return 2;
        }
        int j = i + 1;
        if (j < argc && !parse_double_in_range("lift", argv[j++], LIFT_MIN, LIFT_MAX, &lift)) return 2;
        if (j < argc && !parse_double_in_range("gain", argv[j++], GAIN_MIN, GAIN_MAX, &gain)) return 2;
        if (j < argc && !parse_double_in_range("r",    argv[j++], MULT_MIN, MULT_MAX, &r))    return 2;
        if (j < argc && !parse_double_in_range("g",    argv[j++], MULT_MIN, MULT_MAX, &g))    return 2;
        if (j < argc && !parse_double_in_range("b",    argv[j++], MULT_MIN, MULT_MAX, &b))    return 2;
        if (j != argc) { fprintf(stderr, "Unexpected extra arguments.\n"); return 2; }
    } else {
        const char *preset = argv[i];
        struct preset_vals pv;
        int st = load_preset(preset, preset_path, &pv);
        if (st == 0) {
            fprintf(stderr, "Preset '%s' not found.\n", preset);
            list_all_presets(preset_path);
            return 2;
        }
        if (st < 0) {
            fprintf(stderr, "Error parsing presets for '%s'.\n", preset);
            return 2;
        }
        if (pv.have_crtc) crtc_id = pv.crtc;
        if (!pv.have_gamma) { fprintf(stderr, "Preset '%s' lacks required key 'gamma'.\n", preset); return 2; }
        gamma_pow = pv.gamma;
        if (pv.have_lift) lift = pv.lift;
        if (pv.have_gain) gain = pv.gain;
        if (pv.have_r)    r = pv.r;
        if (pv.have_g)    g = pv.g;
        if (pv.have_b)    b = pv.b;
    }

    /* Open DRM card */
    int fd = -1;
    for (int card = 0; card < 4; card++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", card);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) break;
        if (errno == ENOENT) break;
    }
    if (fd < 0) { perror("open /dev/dri/cardN"); return 1; }

    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    int ret = set_gamma_lut(fd, crtc_id, gamma_pow, lift, gain, r, g, b);
    if (ret) fprintf(stderr, "set_gamma_lut failed: %d\n", ret);

    close(fd);
    return ret ? 1 : 0;
}
