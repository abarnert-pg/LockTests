#include <jni.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <android/log.h>

struct AndroidLogStream {
    std::stringstream ss;
};

template <typename T>
AndroidLogStream &operator<<(AndroidLogStream &s, const T &t) {
    s.ss << t;
    std::string str = s.ss.str();
    if (str.find('\n') != -1) {
        __android_log_write(ANDROID_LOG_WARN, "com.example.locktests", str.c_str());
        s.ss.str("");
    }
    return s;
}

struct posix_error : std::runtime_error {
    int err;
    posix_error() : std::runtime_error(strerror(errno)), err(errno) {}
    posix_error(int e) : std::runtime_error(strerror(e)), err(e) {}
};

void check(int err) {
    if (err) throw posix_error(err);
}

struct PThreadMutexAttr {
    pthread_mutexattr_t attr;
    PThreadMutexAttr() {
        check(pthread_mutexattr_init(&attr));
    }
    ~PThreadMutexAttr() {
        pthread_mutexattr_destroy(&attr);
    }
    PThreadMutexAttr(const PThreadMutexAttr &) = delete;
    PThreadMutexAttr &operator=(const PThreadMutexAttr &) = delete;
    void settype(int type) {
        check(pthread_mutexattr_settype(&attr, type));
    }
};

struct PThreadMutex {
    pthread_mutex_t mutex;
    PThreadMutex() {
        check(pthread_mutex_init(&mutex, nullptr));
    }
    PThreadMutex(int type) {
        PThreadMutexAttr attr;
        attr.settype(type);
        check(pthread_mutex_init(&mutex, &attr.attr));
    }
    ~PThreadMutex() {
        pthread_mutex_destroy(&mutex);
    }
    PThreadMutex(const PThreadMutex &rhs) = delete;
    PThreadMutex &operator=(const PThreadMutex &rhs) = delete;
    bool trylock() {
        int err = pthread_mutex_trylock(&mutex);
        if (err == EBUSY) return false;
        check(err);
        return true;
    }
    void lock() {
        check(pthread_mutex_lock(&mutex));
    }
    void unlock() {
        check(pthread_mutex_unlock(&mutex));
    }
};

struct PThreadMutexNormal : PThreadMutex {
    PThreadMutexNormal() : PThreadMutex(PTHREAD_MUTEX_NORMAL) {}
};

struct PThreadMutexErrorCheck : PThreadMutex {
    PThreadMutexErrorCheck() : PThreadMutex(PTHREAD_MUTEX_ERRORCHECK) {}
};

struct SpinLock {
    int l = 0;
    SpinLock() {}
    ~SpinLock() {}
    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;
    bool trylock() {
        return __sync_bool_compare_and_swap(&l, 0, 1);
    }
    void lock() {
        while(!trylock()) {
            sched_yield();
        }
    }
    void unlock() {
        __sync_bool_compare_and_swap(&l, 1, 0);
    }
};

struct SpinLockSpin : SpinLock {
    void lock() {
        while (!trylock()) {
            // do nothing
        }
    }
};

static int futex(int *uaddr, int futex_op, int val,
                 const struct timespec *timeout)
{
    return syscall(SYS_futex, uaddr, futex_op, val,
                   timeout, NULL, 0);
}

static inline int32_t cmpxchg(int32_t *ptr, int32_t expected, int32_t desired,
                              int success_memorder, int failure_memorder)
{
    __atomic_compare_exchange_n(ptr, &expected, desired,
                                false, success_memorder, failure_memorder);
    return expected;
}

static inline bool futex_trylock(int32_t *lock) {
    return !cmpxchg(lock, 0, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void futex_lock(int32_t *lock) {
    int32_t val = cmpxchg(lock, 0, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    if (!val) {
        return;
    }
    do {
        // NOTE: We could do a weak cmpxchg here, but then we need to check
        // both the success flag and the old value instead of just the old
        // value, and somehow that ends up making things slower. Maybe with the
        // right branch-prediction LIKELY attributes this could be optimized?
        // But it's a lot simpler this way, and already faster than OSSpinLock.
        if (val == 2 || cmpxchg(lock, 1, 2,
                                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) != 0) {
            // NOTE: this can fail spuriously with EAGAIN, but that's fine;
            // the extra cmpxchg might work, so we might as well try it
            // before trying the syscall again.
            futex(lock, FUTEX_WAIT, 2, NULL);
        }
    } while ((val = cmpxchg(lock, 0, 2,
                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) != 0);
}

static inline void futex_unlock(int32_t *lock) {
    if (__atomic_fetch_sub(lock, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
        futex(lock, FUTEX_WAKE, 1, NULL);
    }
}

struct Futex {
    int32_t l = 0;
    Futex() {}
    ~Futex() {}
    Futex(const SpinLock &) = delete;
    Futex &operator=(const Futex &) = delete;
    bool trylock() {
        return futex_trylock(&l);
    }
    void lock() {
        return futex_lock(&l);
    }
    void unlock() {
        return futex_unlock(&l);
    }
};

struct Dummy {
    bool trylock() { return true; }
    void lock() {}
    void unlock() {}
};

std::string procfile() {
    std::stringstream ss;
    ss << "/proc/" << getpid() << "/stat";
    return ss.str();
}

std::pair<int64_t, int64_t> cputime() {
    static std::string fname = procfile();
    std::ifstream f(fname);
    std::string s;
    for (int i=1; i!=14; ++i) {
        f >> s;
    }
    int64_t utime, stime;
    f >> utime >> stime;
    return std::make_pair(utime, stime);
}

struct TimeIt {
    std::pair<int64_t, int64_t> cpu0;
    std::chrono::time_point<std::chrono::high_resolution_clock> t0;
    TimeIt() : cpu0(cputime()), t0(std::chrono::high_resolution_clock::now()) {}
    operator std::string() {
        auto t = std::chrono::high_resolution_clock::now();
        auto d = t - t0;
        auto cpu = cputime();
        auto utime = cpu.first - cpu0.first;
        auto stime = cpu.second - cpu0.second;
        std::stringstream ss;
        ss << std::chrono::duration_cast<std::chrono::duration<double>>(d).count() << "s "
           << utime << " user " << stime << " sys";
        return ss.str();
    }
};

template <typename Mutex, typename Count=int64_t>
std::string test(int64_t reps, int64_t inner, int64_t nthreads, int pshared) {
    TimeIt t;
    volatile Count total = 0;
    std::vector<std::thread> threads;
    Mutex shared;
    for (int64_t i=0; i!=nthreads; ++i) {
        threads.emplace_back([reps, inner, pshared, &total, &shared] {
            Mutex local;
            volatile int64_t count=0;
            for (int64_t j=0; j!=reps; ++j) {
                if (rand() % 100 < pshared) {
                    std::lock_guard<Mutex> guard(shared);
                    for (int64_t k = 0; k != inner; ++k) {
                        ++total;
                    }
                } else {
                    std::lock_guard<Mutex> guard(local);
                    for (int64_t k = 0; k != inner; ++k) {
                        ++count;
                    }
                }
            }
            std::lock_guard<Mutex> guard(shared);
            total += count;
        });
    }
    for (auto &thread: threads) {
        thread.join();
    }
    assert(total == reps * inner * nthreads);
    return t;
}

static std::atomic<bool> done;

std::thread burn(int nthreads) {
    std::thread t([nthreads]{
        std::vector<std::thread> threads;
        for (int i=0; i!=nthreads; ++i) {
            threads.emplace_back([] {
                while (!done) {
                    volatile int count;
                    for (int j = 0; j != 1000; ++j) ++count;
                }
            });
        }
        for (auto &thread: threads) {
            thread.join();
        }
    });
    return std::move(t);
}

struct Burn {
    std::thread t;
    Burn(int nthreads) : t(burn(nthreads)) {}
    ~Burn() { t.join(); }
};

template <typename S1, typename S2>
struct StreamTie {
    S1 &s1;
    S2 &s2;
    StreamTie(S1 &s1_, S2 &s2_) : s1(s1_), s2(s2_) {}
};

template <typename S1, typename S2, typename T>
StreamTie<S1, S2> &operator<<(StreamTie<S1, S2> &s, const T& t) {
    s.s1 << t;
    s.s2 << t;
    return s;
}

template <typename S1, typename S2>
StreamTie<S1, S2> tie_streams(S1 &s1, S2 &s2) { return StreamTie<S1, S2>(s1, s2); }

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_locktests_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    static const int reps = 10000;
    static const int inner = 1000;
    static const int nthreads = 64;
    std::stringstream s;
    AndroidLogStream als;
    auto ss = tie_streams(s, als);
    Burn tburn(8);
    for (int pshared: {0, 1, 2, 5, 10, 25, 50, 75, 90, 99}) {
        ss << "\n" << pshared << "%\n";
        ss << test<std::mutex>(reps, inner, nthreads, pshared) << " std::mutex\n";
        ss << test<PThreadMutexNormal>(reps, inner, nthreads, pshared) << " pthread normal\n";
        ss << test<PThreadMutexErrorCheck>(reps, inner, nthreads, pshared) << " pthread check\n";
        ss << test<SpinLock>(reps, inner, nthreads, pshared) << " spin yield\n";
        //ss << test<SpinLockSpin>(reps, inner, nthreads, pshared) << " spin spin\n";
        ss << test<Futex>(reps, inner, nthreads, pshared) << " futex\n";
        //ss << test<Dummy, std::atomic<int64_t>>(reps, inner, nthreads, pshared) << " atomic\n";
    }
    done = true;
    return env->NewStringUTF(ss.s1.str().c_str());
}
