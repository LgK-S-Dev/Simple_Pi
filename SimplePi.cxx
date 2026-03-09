// pi_stream_bin_only.cpp
// Multithreaded continuous π generator — binary-only checkpointing + chunked compression.
// No decimal conversion. Saves GMP state snapshots to chunk files and compresses chunks on rotation.
//
// Compile:
//   g++ -O2 -std=c++11 pi_stream_bin_only.cpp -lgmp -lz -lpthread -o pi_stream_bin_only
//
// Run:
//   ./pi_stream_bin_only

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <gmp.h>
#include <zlib.h>

//
// Configuration (kept identical to your chosen values)
//
#define THREAD_COUNT 6
#define BLOCK_TERMS 20000
#define WRITE_INTERVAL_TERMS 1500000
#define LOG_INTERVAL_TERMS 400000
#define DIGITS_PER_TERM 14.181647462725477
#define THRESHOLD_BYTES (1ull << 30)
#define CHUNK_BASENAME "pi_chunk"
#define STATE_FILE "pi_stream_state.dat"
#define LOG_FILE "pi_chunk_log.txt"
#define CHUNK_BUFFER_SIZE (8u << 20) // 8 MiB stdio buffer for chunk files
//

static std::string timestamp_now() {
    std::time_t now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return std::string(buf);
}
static std::string timestamp_fname() {
    std::time_t now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&now));
    return std::string(buf);
}
static std::string zero_pad(unsigned int n, int width=4) {
    std::ostringstream ss; ss << std::setw(width) << std::setfill('0') << n; return ss.str();
}

typedef struct {
    long k;
    mpz_t P, Q, T;
} state_t;

typedef struct { mpz_t P, Q, T; } bs_t;

static void bs_init(bs_t *r) { mpz_init(r->P); mpz_init(r->Q); mpz_init(r->T); }
static void bs_clear(bs_t *r) { mpz_clear(r->P); mpz_clear(r->Q); mpz_clear(r->T); }

// Forward
void chudnovsky_bs(long a, long b, bs_t *r);

// ---------- state persistence (binary GMP raw) ----------
static int save_state_file(state_t *s, unsigned int current_chunk_index) {
    FILE *f = fopen(STATE_FILE, "wb");
    if (!f) return 0;
    if (fwrite(&s->k, sizeof(long), 1, f) != 1) { fclose(f); return 0; }
    mpz_out_raw(f, s->P);
    mpz_out_raw(f, s->Q);
    mpz_out_raw(f, s->T);
    if (fwrite(&current_chunk_index, sizeof(unsigned int), 1, f) != 1) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

static int load_state_file(state_t *s, unsigned int *out_chunk_index) {
    FILE *f = fopen(STATE_FILE, "rb");
    if (!f) return 0;
    long k;
    if (fread(&k, sizeof(long), 1, f) != 1) { fclose(f); return 0; }
    s->k = k;
    mpz_init(s->P); mpz_init(s->Q); mpz_init(s->T);
    if (mpz_inp_raw(s->P, f) == 0 || mpz_inp_raw(s->Q, f) == 0 || mpz_inp_raw(s->T, f) == 0) {
        mpz_clear(s->P); mpz_clear(s->Q); mpz_clear(s->T);
        fclose(f);
        return 0;
    }
    unsigned int ci;
    if (fread(&ci, sizeof(unsigned int), 1, f) != 1) { fclose(f); return 0; }
    *out_chunk_index = ci;
    fclose(f);
    return 1;
}

static void state_init(state_t *s) {
    s->k = 0;
    mpz_init_set_ui(s->P, 1);
    mpz_init_set_ui(s->Q, 1);
    mpz_init_set_ui(s->T, 0);
}
static void state_clear(state_t *s) { mpz_clear(s->P); mpz_clear(s->Q); mpz_clear(s->T); }

// ---------- chunk filename helpers ----------
static std::string chunk_bin_name(unsigned int idx) {
    std::ostringstream ss; ss << CHUNK_BASENAME << "_" << zero_pad(idx,4) << ".bin"; return ss.str();
}
static std::string chunk_gz_name(unsigned int idx) {
    std::ostringstream ss; ss << CHUNK_BASENAME << "_" << zero_pad(idx,4) << ".bin.gz"; return ss.str();
}

static uint64_t file_size_bytes(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return (uint64_t)st.st_size;
    return 0;
}

// Compress file src -> dst (gzip) using streaming zlib (gzopen)
static bool compress_file_gzip(const std::string &src, const std::string &dst) {
    const size_t BUF = 1 << 20; // 1 MiB buffer
    FILE *in = fopen(src.c_str(), "rb");
    if (!in) return false;
    gzFile out = gzopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(BUF);
    size_t r;
    while ((r = fread(buf.data(), 1, BUF, in)) > 0) {
        int w = gzwrite(out, buf.data(), (unsigned int)r);
        if (w == 0) { gzclose(out); fclose(in); return false; }
    }
    gzclose(out);
    fclose(in);
    return true;
}

// Logging
static std::mutex log_mutex;
static void log_printf(const std::string &s) {
    std::lock_guard<std::mutex> lk(log_mutex);
    std::ofstream lf(LOG_FILE, std::ios::app);
    if (lf.is_open()) {
        lf << "[" << timestamp_now() << "] " << s << std::endl;
    }
    // also stdout for immediate feedback
    std::printf("[%s] %s\n", timestamp_now().c_str(), s.c_str());
}

// ---------- chunk file header & snapshot write ----------
// Each chunk starts with ASCII header "PI_CHUNK_V1\n" and an ISO timestamp and reserved bytes.
// Each snapshot record appended: uint64_t term_index, uint64_t epoch_seconds, then mpz_out_raw(P), mpz_out_raw(Q), mpz_out_raw(T).
// Use FILE* with large stdio buffer for efficiency.

static bool write_chunk_header(FILE *fp) {
    if (!fp) return false;
    // simple ascii header
    const char *magic = "PI_CHUNK_V1\n";
    if (fwrite(magic, 1, strlen(magic), fp) != strlen(magic)) return false;
    std::string ts = timestamp_now() + "\n";
    if (fwrite(ts.c_str(), 1, ts.size(), fp) != ts.size()) return false;
    // reserve 256 bytes padding for future metadata (write zeros)
    char pad[256]; memset(pad, 0, sizeof(pad));
    if (fwrite(pad, 1, sizeof(pad), fp) != sizeof(pad)) return false;
    return true;
}

// Append snapshot: term index + epoch + mpz_out_raw P,Q,T
static bool append_snapshot_to_chunk(FILE *fp, state_t *s) {
    if (!fp || !s) return false;
    uint64_t term = (uint64_t)s->k;
    uint64_t epoch = (uint64_t)std::time(nullptr);
    if (fwrite(&term, sizeof(uint64_t), 1, fp) != 1) return false;
    if (fwrite(&epoch, sizeof(uint64_t), 1, fp) != 1) return false;
    // write mpz raw data sequentially
    mpz_out_raw(fp, s->P);
    mpz_out_raw(fp, s->Q);
    mpz_out_raw(fp, s->T);
    // do not fflush here for performance; caller can do fflush if needed
    return true;
}

// ---------- Binary-splitting implementation ----------
void chudnovsky_bs(long a, long b, bs_t *r) {
    if (b - a == 1) {
        if (a == 0) {
            mpz_set_ui(r->P, 1);
            mpz_set_ui(r->Q, 1);
            mpz_set_ui(r->T, 13591409);
        } else {
            mpz_set_si(r->P, (long)(6*a - 5));
            mpz_mul_si(r->P, r->P, (long)(2*a - 1));
            mpz_mul_si(r->P, r->P, (long)(6*a - 1));

            mpz_t tmp; mpz_init(tmp);
            mpz_set_ui(tmp, (unsigned long)a);
            mpz_pow_ui(tmp, tmp, 3);
            mpz_mul_ui(tmp, tmp, 640320u);
            mpz_mul_ui(tmp, tmp, 640320u);
            mpz_mul_ui(tmp, tmp, 640320u);
            mpz_set(r->Q, tmp);
            mpz_clear(tmp);

            mpz_set_ui(r->T, 545140134u);
            mpz_mul_ui(r->T, r->T, (unsigned long)a);
            mpz_add_ui(r->T, r->T, 13591409u);
            mpz_mul(r->T, r->T, r->P);
            if (a & 1) mpz_neg(r->T, r->T);
        }
        return;
    }
    long m = (a + b) >> 1;
    bs_t left, right; bs_init(&left); bs_init(&right);
    chudnovsky_bs(a, m, &left);
    chudnovsky_bs(m, b, &right);
    mpz_mul(r->P, left.P, right.P);
    mpz_mul(r->Q, left.Q, right.Q);
    mpz_t tmp; mpz_init(tmp);
    mpz_mul(r->T, left.T, right.Q);
    mpz_mul(tmp, left.P, right.T);
    mpz_add(r->T, r->T, tmp);
    mpz_clear(tmp);
    bs_clear(&left); bs_clear(&right);
}

// ---------- Main loop ----------
int main() {
    log_printf("Starting pi_stream_bin_only (binary-only checkpointing).");

    // load state if exists
    state_t state;
    unsigned int current_chunk_index = 1;
    bool have_state = load_state_file(&state, &current_chunk_index);
    if (!have_state) {
        state_init(&state);
        current_chunk_index = 1;
        save_state_file(&state, current_chunk_index);
        log_printf("Fresh start; state initialized.");
    } else {
        std::ostringstream s; s << "Resuming from term " << state.k << ", chunk=" << current_chunk_index;
        log_printf(s.str());
    }

    // open or create current chunk file for append, create header if new
    std::string chunk_path = chunk_bin_name(current_chunk_index);
    FILE *chunk_fp = fopen(chunk_path.c_str(), "ab+");
    if (!chunk_fp) {
        std::fprintf(stderr, "Failed to open chunk file %s: %s\n", chunk_path.c_str(), strerror(errno));
        return 1;
    }
    // set large stdio buffer
    static char io_buf[CHUNK_BUFFER_SIZE];
    setvbuf(chunk_fp, io_buf, _IOFBF, CHUNK_BUFFER_SIZE);

    // if file is empty, write header
    uint64_t cur_size = file_size_bytes(chunk_path);
    if (cur_size == 0) {
        if (!write_chunk_header(chunk_fp)) {
            std::fprintf(stderr, "Failed to write chunk header\n");
            fclose(chunk_fp);
            return 1;
        }
        fflush(chunk_fp);
        cur_size = file_size_bytes(chunk_path);
    }

    unsigned int num_threads = THREAD_COUNT;

    // Main continuous loop
    while (true) {
        long start_k = state.k;
        long end_k = state.k + BLOCK_TERMS;

        // split range into per-thread subranges
        std::vector<std::pair<long,long>> ranges;
        ranges.reserve(num_threads);
        long per = BLOCK_TERMS / (long)num_threads;
        long rem = BLOCK_TERMS % (long)num_threads;
        long cur = start_k;
        for (unsigned int i = 0; i < num_threads; ++i) {
            long s = cur;
            long add = per + ((long)i < rem ? 1 : 0);
            long e = s + add;
            if (s >= end_k) e = s;
            ranges.emplace_back(s,e);
            cur = e;
        }

        // per-thread results
        std::vector<bs_t> results(ranges.size());
        for (size_t i = 0; i < results.size(); ++i) bs_init(&results[i]);

        // launch threads
        std::vector<std::thread> ths;
        ths.reserve(ranges.size());
        for (size_t i = 0; i < ranges.size(); ++i) {
            long a = ranges[i].first, b = ranges[i].second;
            ths.emplace_back([a,b,&results,i]() {
                if (b <= a) {
                    mpz_set_ui(results[i].P,1);
                    mpz_set_ui(results[i].Q,1);
                    mpz_set_ui(results[i].T,0);
                } else {
                    chudnovsky_bs(a,b,&results[i]);
                }
            });
        }
        for (auto &t: ths) t.join();

        // combine results (divide & conquer)
        bs_t block; bs_init(&block);
        if (results.size() == 0) {
            mpz_set_ui(block.P,1); mpz_set_ui(block.Q,1); mpz_set_ui(block.T,0);
        } else {
            std::function<void(size_t,size_t,bs_t*)> combine = [&](size_t l, size_t r, bs_t *out) {
                if (l == r) {
                    mpz_set(out->P, results[l].P);
                    mpz_set(out->Q, results[l].Q);
                    mpz_set(out->T, results[l].T);
                    return;
                }
                size_t m = (l + r) >> 1;
                bs_t L; bs_init(&L); bs_t R; bs_init(&R);
                combine(l, m, &L);
                combine(m+1, r, &R);
                mpz_mul(out->P, L.P, R.P);
                mpz_mul(out->Q, L.Q, R.Q);
                mpz_t tmp; mpz_init(tmp);
                mpz_mul(out->T, L.T, R.Q);
                mpz_mul(tmp, L.P, R.T);
                mpz_add(out->T, out->T, tmp);
                mpz_clear(tmp);
                bs_clear(&L); bs_clear(&R);
            };
            combine(0, results.size()-1, &block);
        }

        // free per-thread results
        for (size_t i = 0; i < results.size(); ++i) bs_clear(&results[i]);

        // fold block into global state: state.P *= block.P, etc.
        mpz_t newP,newQ,newT,tmp; mpz_inits(newP,newQ,newT,tmp,NULL);
        mpz_mul(newP, state.P, block.P);
        mpz_mul(newQ, state.Q, block.Q);
        mpz_mul(newT, state.T, block.Q);
        mpz_mul(tmp, state.P, block.T);
        mpz_add(newT, newT, tmp);
        mpz_set(state.P, newP);
        mpz_set(state.Q, newQ);
        mpz_set(state.T, newT);
        state.k = end_k;
        mpz_clears(newP,newQ,newT,tmp,NULL);
        bs_clear(&block);

        // Periodically write a binary snapshot into the current chunk
        if ((state.k % WRITE_INTERVAL_TERMS) == 0) {
            // append snapshot: term epoch mpz_out_raw(P) Q T
            // protect chunk operations with a mutex
            static std::mutex chunk_mutex;
            {
                std::lock_guard<std::mutex> lk(chunk_mutex);
                // Seek to end for append (fp opened append+)
                fseek(chunk_fp, 0, SEEK_END);
                if (!append_snapshot_to_chunk(chunk_fp, &state)) {
                    log_printf("Error: failed to append snapshot to chunk.");
                    // attempt to save state and exit
                    save_state_file(&state, current_chunk_index);
                    fclose(chunk_fp);
                    state_clear(&state);
                    return 2;
                }
                fflush(chunk_fp);
            }

            // save top-level state file for fast resume
            save_state_file(&state, current_chunk_index);

            // check chunk size and rotate if necessary
            cur_size = file_size_bytes(chunk_path);
            if (cur_size >= THRESHOLD_BYTES) {
                // close and compress
                fclose(chunk_fp);
                std::string gz = chunk_gz_name(current_chunk_index);
                bool ok = compress_file_gzip(chunk_path, gz);
                uint64_t comp_bytes = ok ? file_size_bytes(gz) : 0;
                uint64_t uncompressed = file_size_bytes(chunk_path);
                // log event
                {
                    std::ostringstream ss;
                    ss << "Rotated chunk " << zero_pad(current_chunk_index,4)
                       << " uncompressed=" << uncompressed << " compressed=" << comp_bytes;
                    log_printf(ss.str());
                }
                // remove original .bin if compression succeeded
                if (ok) unlink(chunk_path.c_str());
                // advance chunk index and open a new file (and write header)
                current_chunk_index++;
                chunk_path = chunk_bin_name(current_chunk_index);
                chunk_fp = fopen(chunk_path.c_str(), "ab+");
                if (!chunk_fp) {
                    std::fprintf(stderr, "Failed to open new chunk file %s: %s\n", chunk_path.c_str(), strerror(errno));
                    state_clear(&state);
                    return 3;
                }
                setvbuf(chunk_fp, io_buf, _IOFBF, CHUNK_BUFFER_SIZE);
                // write header to new chunk
                if (!write_chunk_header(chunk_fp)) {
                    std::fprintf(stderr, "Failed to write header to new chunk\n");
                    fclose(chunk_fp);
                    state_clear(&state);
                    return 4;
                }
                fflush(chunk_fp);
            }
        }

        if ((state.k % LOG_INTERVAL_TERMS) == 0) {
            double approx_digits = state.k * DIGITS_PER_TERM;
            std::ostringstream ss;
            ss << "Terms: " << state.k << " (~" << std::fixed << std::setprecision(0) << approx_digits
               << " digits)  chunk=" << current_chunk_index;
            log_printf(ss.str());
        }

        // periodic save
        save_state_file(&state, current_chunk_index);
    }

    // unreachable
    state_clear(&state);
    return 0;
}