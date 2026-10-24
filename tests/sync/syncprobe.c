/* Exercises Windows synchronization objects the way games do and times it.
 * Prints one line per test, "PROBE sync OK" when every check held, and the time each benchmark took, so a
 * Wine build can be compared with and without in-process synchronization (WINEOSYNC=1).
 * Exit code 0 when all checks passed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq.QuadPart;
}

/* --- benchmarks ------------------------------------------------------------------------------------- */

#define PINGPONG 100000
static HANDLE ping, pong;

static DWORD WINAPI pong_thread(void *arg)
{
    int i;
    for (i = 0; i < PINGPONG; i++)
    {
        WaitForSingleObject(ping, INFINITE);
        SetEvent(pong);
    }
    return 0;
}

static void bench_pingpong(void)
{
    HANDLE thread;
    double t;
    int i;

    ping = CreateEventW(NULL, FALSE, FALSE, NULL);
    pong = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, pong_thread, NULL, 0, NULL);
    t = now_ms();
    for (i = 0; i < PINGPONG; i++)
    {
        SetEvent(ping);
        if (WaitForSingleObject(pong, 5000) != WAIT_OBJECT_0) { printf("FAILED pingpong stalled at %d\n", i); failures++; break; }
    }
    t = now_ms() - t;
    WaitForSingleObject(thread, INFINITE);
    printf("bench event ping-pong: %d round trips in %.0f ms (%.1f us each)\n", PINGPONG, t, t * 1000 / PINGPONG);
    CloseHandle(thread); CloseHandle(ping); CloseHandle(pong);
}

#define MUTEX_THREADS 8
#define MUTEX_ITERS 20000
static HANDLE mutex;
static volatile LONG counter, inside;

static DWORD WINAPI mutex_thread(void *arg)
{
    int i;
    for (i = 0; i < MUTEX_ITERS; i++)
    {
        if (WaitForSingleObject(mutex, 5000) != WAIT_OBJECT_0) { failures++; return 1; }
        if (InterlockedIncrement(&inside) != 1) failures++;
        counter++;
        InterlockedDecrement(&inside);
        ReleaseMutex(mutex);
    }
    return 0;
}

static void bench_mutex(void)
{
    HANDLE threads[MUTEX_THREADS];
    double t;
    int i;

    mutex = CreateMutexW(NULL, FALSE, NULL);
    counter = 0;
    t = now_ms();
    for (i = 0; i < MUTEX_THREADS; i++) threads[i] = CreateThread(NULL, 0, mutex_thread, NULL, 0, NULL);
    WaitForMultipleObjects(MUTEX_THREADS, threads, TRUE, INFINITE);
    t = now_ms() - t;
    CHECK(counter == MUTEX_THREADS * MUTEX_ITERS);
    printf("bench mutex contention: %d threads x %d in %.0f ms\n", MUTEX_THREADS, MUTEX_ITERS, t);
    for (i = 0; i < MUTEX_THREADS; i++) CloseHandle(threads[i]);
    CloseHandle(mutex);
}

#define SEM_ITEMS 200000
static HANDLE sem_full, sem_empty;
static volatile LONG produced, consumed;

static DWORD WINAPI producer(void *arg)
{
    int i;
    for (i = 0; i < SEM_ITEMS / 2; i++)
    {
        WaitForSingleObject(sem_empty, INFINITE);
        InterlockedIncrement(&produced);
        ReleaseSemaphore(sem_full, 1, NULL);
    }
    return 0;
}

static DWORD WINAPI consumer(void *arg)
{
    int i;
    for (i = 0; i < SEM_ITEMS / 2; i++)
    {
        WaitForSingleObject(sem_full, INFINITE);
        InterlockedIncrement(&consumed);
        ReleaseSemaphore(sem_empty, 1, NULL);
    }
    return 0;
}

static void bench_semaphore(void)
{
    HANDLE threads[4];
    double t;
    int i;

    sem_full = CreateSemaphoreW(NULL, 0, 64, NULL);
    sem_empty = CreateSemaphoreW(NULL, 64, 64, NULL);
    t = now_ms();
    threads[0] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    threads[2] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    threads[3] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    t = now_ms() - t;
    CHECK(produced == SEM_ITEMS && consumed == SEM_ITEMS);
    printf("bench semaphore queue: %d items in %.0f ms\n", SEM_ITEMS, t);
    for (i = 0; i < 4; i++) CloseHandle(threads[i]);
    CloseHandle(sem_full); CloseHandle(sem_empty);
}

#define ANY_ROUNDS 50000
static HANDLE any_events[4], any_ack;

static DWORD WINAPI any_thread(void *arg)
{
    int i;
    for (i = 0; i < ANY_ROUNDS; i++)
    {
        SetEvent(any_events[i % 4]);
        WaitForSingleObject(any_ack, INFINITE);
    }
    return 0;
}

static void bench_wait_any(void)
{
    HANDLE thread;
    double t;
    int i;

    for (i = 0; i < 4; i++) any_events[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
    any_ack = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, any_thread, NULL, 0, NULL);
    t = now_ms();
    for (i = 0; i < ANY_ROUNDS; i++)
    {
        DWORD r = WaitForMultipleObjects(4, any_events, FALSE, 5000);
        if (r != WAIT_OBJECT_0 + i % 4) { printf("FAILED wait any round %d: %lu\n", i, r); failures++; break; }
        SetEvent(any_ack);
    }
    t = now_ms() - t;
    WaitForSingleObject(thread, INFINITE);
    printf("bench wait any of 4: %d rounds in %.0f ms\n", ANY_ROUNDS, t);
    CloseHandle(thread); CloseHandle(any_ack);
    for (i = 0; i < 4; i++) CloseHandle(any_events[i]);
}

#define ALL_ROUNDS 20000
static HANDLE all_events[2], all_ack;

static DWORD WINAPI all_thread(void *arg)
{
    int i;
    for (i = 0; i < ALL_ROUNDS; i++)
    {
        SetEvent(all_events[0]);
        SetEvent(all_events[1]);
        WaitForSingleObject(all_ack, INFINITE);
    }
    return 0;
}

static void bench_wait_all(void)
{
    HANDLE thread;
    double t;
    int i;

    all_events[0] = CreateEventW(NULL, FALSE, FALSE, NULL);
    all_events[1] = CreateEventW(NULL, FALSE, FALSE, NULL);
    all_ack = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, all_thread, NULL, 0, NULL);
    t = now_ms();
    for (i = 0; i < ALL_ROUNDS; i++)
    {
        if (WaitForMultipleObjects(2, all_events, TRUE, 5000) != WAIT_OBJECT_0) { printf("FAILED wait all round %d\n", i); failures++; break; }
        SetEvent(all_ack);
    }
    t = now_ms() - t;
    WaitForSingleObject(thread, INFINITE);
    printf("bench wait all of 2: %d rounds in %.0f ms\n", ALL_ROUNDS, t);
    CloseHandle(thread); CloseHandle(all_ack); CloseHandle(all_events[0]); CloseHandle(all_events[1]);
}

#define PROC_ROUNDS 50000

static int child_main(void)
{
    HANDLE ping = OpenEventW(EVENT_ALL_ACCESS, FALSE, L"EscAppSyncPing"), pong = OpenEventW(EVENT_ALL_ACCESS, FALSE, L"EscAppSyncPong");
    int i;
    if (!ping || !pong) return 1;
    for (i = 0; i < PROC_ROUNDS; i++)
    {
        if (WaitForSingleObject(ping, 10000) != WAIT_OBJECT_0) return 2;
        SetEvent(pong);
    }
    return 0;
}

static void bench_processes(void)
{
    HANDLE ping = CreateEventW(NULL, FALSE, FALSE, L"EscAppSyncPing"), pong = CreateEventW(NULL, FALSE, FALSE, L"EscAppSyncPong");
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    WCHAR cmd[MAX_PATH + 16];
    DWORD code = 99;
    double t;
    int i;

    wcscpy(cmd, L"\"");
    GetModuleFileNameW(NULL, cmd + 1, MAX_PATH);
    wcscat(cmd, L"\" child");
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("FAILED CreateProcess %lu\n", GetLastError()); failures++; return; }
    t = now_ms();
    for (i = 0; i < PROC_ROUNDS; i++)
    {
        SetEvent(ping);
        if (WaitForSingleObject(pong, 10000) != WAIT_OBJECT_0) { printf("FAILED process ping-pong at %d\n", i); failures++; break; }
    }
    t = now_ms() - t;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    CHECK(code == 0);
    printf("bench process ping-pong: %d round trips in %.0f ms (%.1f us each)\n", PROC_ROUNDS, t, t * 1000 / PROC_ROUNDS);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(ping); CloseHandle(pong);
}

/* --- semantics -------------------------------------------------------------------------------------- */

static DWORD WINAPI abandon_thread(void *arg)
{
    WaitForSingleObject((HANDLE)arg, INFINITE);
    return 0; /* exits holding the mutex */
}

static DWORD WINAPI pulse_waiter(void *arg)
{
    return WaitForSingleObject((HANDLE)arg, 3000);
}

static void check_semantics(void)
{
    HANDLE ev, ev2, m, s, thread, handles[2];
    LONG prev;
    DWORD r, code;
    double t;

    /* manual vs auto reset, timeouts */
    ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
    t = now_ms();
    CHECK(WaitForSingleObject(ev, 100) == WAIT_TIMEOUT);
    t = now_ms() - t;
    CHECK(t >= 90 && t < 1000);
    SetEvent(ev);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
    ResetEvent(ev);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
    ev2 = CreateEventW(NULL, FALSE, TRUE, NULL);
    CHECK(WaitForSingleObject(ev2, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(ev2, 0) == WAIT_TIMEOUT);

    /* wait any / wait all */
    handles[0] = ev; handles[1] = ev2;
    SetEvent(ev2);
    CHECK(WaitForMultipleObjects(2, handles, FALSE, 0) == WAIT_OBJECT_0 + 1);
    CHECK(WaitForMultipleObjects(2, handles, TRUE, 0) == WAIT_TIMEOUT);
    SetEvent(ev); SetEvent(ev2);
    CHECK(WaitForMultipleObjects(2, handles, TRUE, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(ev2, 0) == WAIT_TIMEOUT); /* auto event consumed by wait all */
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);  /* manual one still set */

    /* pulse wakes a waiter and leaves the event reset */
    ResetEvent(ev);
    thread = CreateThread(NULL, 0, pulse_waiter, ev, 0, NULL);
    Sleep(200);
    PulseEvent(ev);
    WaitForSingleObject(thread, INFINITE);
    GetExitCodeThread(thread, &code);
    CHECK(code == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
    CloseHandle(thread);

    /* recursive mutex, wrong-owner release, abandonment */
    m = CreateMutexW(NULL, TRUE, NULL);
    CHECK(WaitForSingleObject(m, 0) == WAIT_OBJECT_0);
    CHECK(ReleaseMutex(m));
    CHECK(ReleaseMutex(m));
    CHECK(!ReleaseMutex(m));
    thread = CreateThread(NULL, 0, abandon_thread, m, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CHECK(WaitForSingleObject(m, 1000) == WAIT_ABANDONED);
    CHECK(ReleaseMutex(m));
    CloseHandle(thread);

    /* semaphore counts and limits */
    s = CreateSemaphoreW(NULL, 1, 2, NULL);
    CHECK(ReleaseSemaphore(s, 1, &prev) && prev == 1);
    CHECK(!ReleaseSemaphore(s, 1, &prev) && GetLastError() == ERROR_TOO_MANY_POSTS);
    CHECK(WaitForSingleObject(s, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(s, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(s, 0) == WAIT_TIMEOUT);

    /* named objects are shared */
    {
        HANDLE a = CreateEventW(NULL, TRUE, FALSE, L"EscAppSyncProbe"), b = OpenEventW(EVENT_ALL_ACCESS, FALSE, L"EscAppSyncProbe");
        CHECK(a && b);
        SetEvent(a);
        CHECK(WaitForSingleObject(b, 0) == WAIT_OBJECT_0);
        CloseHandle(a); CloseHandle(b);
    }

    /* alertable wait gets an APC */
    r = WaitForSingleObjectEx(ev2, 0, TRUE);
    CHECK(r == WAIT_TIMEOUT);
    CloseHandle(ev); CloseHandle(ev2); CloseHandle(m); CloseHandle(s);
}

static HANDLE ready;

static DWORD WINAPI hold_and_sleep(void *arg)
{
    WaitForSingleObject((HANDLE)arg, INFINITE);
    SetEvent(ready);
    Sleep(INFINITE);
    return 0;
}

static DWORD WINAPI wait_forever(void *arg)
{
    SetEvent(ready);
    WaitForSingleObject((HANDLE)arg, INFINITE);
    return 0;
}

static DWORD WINAPI wait_two_seconds(void *arg)
{
    return WaitForSingleObject((HANDLE)arg, 2000);
}

static void check_thread_death(void)
{
    HANDLE m = CreateMutexW(NULL, FALSE, NULL), s = CreateSemaphoreW(NULL, 0, 10, NULL), thread, waiters[3], h[2], m2;
    DWORD code;
    int i;

    ready = CreateEventW(NULL, FALSE, FALSE, NULL);

    /* killed while holding a mutex: abandoned */
    thread = CreateThread(NULL, 0, hold_and_sleep, m, 0, NULL);
    WaitForSingleObject(ready, INFINITE);
    TerminateThread(thread, 1);
    WaitForSingleObject(thread, INFINITE);
    CHECK(WaitForSingleObject(m, 1000) == WAIT_ABANDONED);
    CHECK(ReleaseMutex(m));
    CloseHandle(thread);

    /* killed while waiting: the semaphore count isn't lost to it */
    thread = CreateThread(NULL, 0, wait_forever, s, 0, NULL);
    WaitForSingleObject(ready, INFINITE);
    Sleep(100);
    TerminateThread(thread, 1);
    WaitForSingleObject(thread, INFINITE);
    ReleaseSemaphore(s, 1, NULL);
    CHECK(WaitForSingleObject(s, 1000) == WAIT_OBJECT_0);
    CloseHandle(thread);

    /* last handle closed while a thread waits: the wait keeps the mutex alive, and its owner's death abandons it */
    {
        HANDLE dup, owner;
        m2 = CreateMutexW(NULL, FALSE, NULL);
        owner = CreateThread(NULL, 0, hold_and_sleep, m2, 0, NULL);
        WaitForSingleObject(ready, INFINITE);
        DuplicateHandle(GetCurrentProcess(), m2, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        thread = CreateThread(NULL, 0, wait_two_seconds, dup, 0, NULL);
        Sleep(200);
        CloseHandle(m2);
        CloseHandle(dup);  /* the waiter's handle too: only its wait keeps the mutex now */
        Sleep(100);
        TerminateThread(owner, 1);
        WaitForSingleObject(owner, INFINITE);
        WaitForSingleObject(thread, INFINITE);
        GetExitCodeThread(thread, &code);
        CHECK(code == WAIT_ABANDONED);
        CloseHandle(owner); CloseHandle(thread);
    }

    /* one release of 3 wakes 3 waiters */
    for (i = 0; i < 3; i++) waiters[i] = CreateThread(NULL, 0, wait_two_seconds, s, 0, NULL);
    Sleep(200);
    ReleaseSemaphore(s, 3, NULL);
    for (i = 0; i < 3; i++)
    {
        WaitForSingleObject(waiters[i], INFINITE);
        GetExitCodeThread(waiters[i], &code);
        CHECK(code == WAIT_OBJECT_0);
        CloseHandle(waiters[i]);
    }

    /* wait all: a mutex and an event; and the same handle twice is refused */
    h[0] = m; h[1] = CreateEventW(NULL, TRUE, TRUE, NULL);
    CHECK(WaitForMultipleObjects(2, h, TRUE, 0) == WAIT_OBJECT_0);
    CHECK(ReleaseMutex(m));
    /* Windows (and ntsync, and osync) refuse a WaitAll naming an object twice; Wine's server doesn't */
    h[1] = h[0];
    if (getenv("WINEOSYNC") && atoi(getenv("WINEOSYNC")))
        CHECK(WaitForMultipleObjects(2, h, TRUE, 0) == WAIT_FAILED && GetLastError() == ERROR_INVALID_PARAMETER);

    CloseHandle(m); CloseHandle(s); CloseHandle(ready);
}

/* A thread killed while waiting must not leave its handle value slow for the next object that gets it (Wine's
 * handle cache once kept such a value out of the cache for good, sending every call on it to the server). */
static double time_set_reset(HANDLE ev, int n)
{
    double t = now_ms();
    int i;
    for (i = 0; i < n; i++) { SetEvent(ev); ResetEvent(ev); }
    return now_ms() - t;
}

static void check_cache_after_kill(void)
{
    HANDLE ev, ev2, fresh, thread;
    double t_reused, t_fresh;

    ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, wait_forever, ev, 0, NULL);
    WaitForSingleObject(ready, INFINITE);
    Sleep(100);
    TerminateThread(thread, 1);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(ev);
    ev2 = CreateEventW(NULL, TRUE, FALSE, NULL);  /* takes the freed handle value */
    fresh = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ev2 != ev) printf("note: the handle value wasn't reused (%p, %p)\n", ev, ev2);
    time_set_reset(ev2, 100); time_set_reset(fresh, 100);
    t_reused = time_set_reset(ev2, 20000);
    t_fresh = time_set_reset(fresh, 20000);
    printf("cache after a killed waiter: reused handle %.0f ms, fresh %.0f ms\n", t_reused, t_fresh);
    CHECK(t_reused < 3 * t_fresh + 20);
    CloseHandle(ev2); CloseHandle(fresh); CloseHandle(ready);
}

static void CALLBACK apc(ULONG_PTR arg) { *(int *)arg = 1; }

static void check_apc(void)
{
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    int called = 0;
    QueueUserAPC(apc, GetCurrentThread(), (ULONG_PTR)&called);
    CHECK(WaitForSingleObjectEx(ev, 1000, TRUE) == WAIT_IO_COMPLETION);
    CHECK(called);
    CloseHandle(ev);
}

/* --- differential fuzz -------------------------------------------------------------------------------- */

/* A seeded, single-threaded sequence of calls whose every result and state goes into a hash. The server's
 * implementation is the reference: run.sh checks that every mode gives the same hash. */
typedef LONG (WINAPI *query_fn)(HANDLE, int, void *, ULONG, ULONG *);
static query_fn pNtQueryEvent, pNtQuerySemaphore, pNtQueryMutant;
static unsigned int fuzz_hash = 2166136261u;
static unsigned int fuzz_seed;

static void mix(unsigned int v)
{
    int i;
    for (i = 0; i < 4; i++) { fuzz_hash ^= (v >> (i * 8)) & 0xff; fuzz_hash *= 16777619u; }
}

static unsigned int rnd(unsigned int n)
{
    fuzz_seed = fuzz_seed * 1103515245u + 12345u;
    return (fuzz_seed >> 8) % n;
}

#define FUZZ_OBJS 10

static int fuzz_main(unsigned int seed, int steps)
{
    HANDLE h[FUZZ_OBJS], list[6];
    int type[FUZZ_OBJS], i, n;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

    pNtQueryEvent = (query_fn)GetProcAddress(ntdll, "NtQueryEvent");
    pNtQuerySemaphore = (query_fn)GetProcAddress(ntdll, "NtQuerySemaphore");
    pNtQueryMutant = (query_fn)GetProcAddress(ntdll, "NtQueryMutant");
    fuzz_seed = seed;
    for (i = 0; i < FUZZ_OBJS; i++)
    {
        type[i] = rnd(4);
        switch (type[i])
        {
        case 0: h[i] = CreateEventW(NULL, TRUE, rnd(2), NULL); break;
        case 1: h[i] = CreateEventW(NULL, FALSE, rnd(2), NULL); break;
        case 2: { LONG max = 1 + rnd(4); h[i] = CreateSemaphoreW(NULL, rnd(max + 1), max, NULL); break; }
        case 3: h[i] = CreateMutexW(NULL, rnd(2), NULL); break;
        }
    }
    for (n = 0; n < steps; n++)
    {
        int k = rnd(FUZZ_OBJS), op = rnd(12), count, j;
        LONG prev = -77;
        DWORD r;

        SetLastError(0xdeadbeef);
        switch (op)
        {
        case 0: r = SetEvent(h[k]); break;
        case 1: r = ResetEvent(h[k]); break;
        case 2: r = PulseEvent(h[k]); break;
        case 3: r = ReleaseSemaphore(h[k], 1 + rnd(3), &prev); mix(prev); break;
        case 4: r = ReleaseMutex(h[k]); break;
        case 5: r = WaitForSingleObject(h[k], 0); break;
        case 6: case 7:
            count = 1 + rnd(6);
            for (j = 0; j < count; j++) list[j] = h[rnd(FUZZ_OBJS)];
            r = WaitForMultipleObjects(count, list, FALSE, 0);
            break;
        case 8:
            count = 1 + rnd(4);
            for (j = 0; j < count; j++)  /* distinct: with repeats Wine's server differs from Windows */
            {
                int l;
                do { list[j] = h[rnd(FUZZ_OBJS)]; for (l = 0; l < j && list[l] != list[j]; l++); } while (l < j);
            }
            r = WaitForMultipleObjects(count, list, TRUE, 0);
            break;
        case 9:
            r = SignalObjectAndWait(h[k], h[rnd(FUZZ_OBJS)], 0, FALSE);
            break;
        case 10:
            r = WaitForSingleObject(h[k], 1 + rnd(3));  /* short real timeout */
            break;
        default:
        {
            ULONG info[4] = {0}, len = 0;
            LONG st;
            if (type[k] < 2) st = pNtQueryEvent(h[k], 0, info, sizeof(info), &len);
            else if (type[k] == 2) st = pNtQuerySemaphore(h[k], 0, info, sizeof(info), &len);
            else st = pNtQueryMutant(h[k], 0, info, sizeof(info), &len);
            mix(st); mix(info[0]); mix(info[1]); mix(info[2]); mix(len);
            r = 0;
            break;
        }
        }
        mix(op); mix(k); mix(r); mix(GetLastError());
    }
    printf("fuzz seed %u steps %d hash %08x\n", seed, steps, fuzz_hash);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fuzz"))
    {
        setvbuf(stdout, NULL, _IONBF, 0);
        return fuzz_main(argc > 2 ? strtoul(argv[2], NULL, 0) : 1, argc > 3 ? atoi(argv[3]) : 20000);
    }
    if (argc > 1 && !strcmp(argv[1], "child")) return child_main();
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("syncprobe %d-bit\n", (int)sizeof(void *) * 8);
    if (argc < 2 || strcmp(argv[1], "bench"))  /* "bench": timings only */
    {
        check_semantics();
        check_apc();
    }
    bench_pingpong();
    bench_mutex();
    bench_semaphore();
    bench_wait_any();
    bench_wait_all();
    bench_processes();
    /* Last: Wine's handle cache keeps a reference that a thread killed while waiting never drops, so a reused
     * handle value isn't cached any more and every call on it asks the server; timings after this mean little. */
    if (argc < 2 || strcmp(argv[1], "bench"))
    {
        check_thread_death();
        check_cache_after_kill();
    }
    printf(failures ? "PROBE sync FAILED (%d)\n" : "PROBE sync OK\n", failures);
    return failures ? 1 : 0;
}
