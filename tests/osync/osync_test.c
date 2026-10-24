/* Unit and stress tests for osync's shared-memory synchronization (include/wine/osync.h), built as a plain macOS
 * program, without Wine. tests/osync/run.sh builds it for arm64 and x86_64 and with sanitizers.
 *
 * The test plays the server's part where osync needs one: it carves cells out of an anonymous shared mapping (which
 * survives fork, for the cross-process tests) and, for a "dead" process, does what wineserver does when a thread
 * dies. Every test checks invariants, and a watchdog turns a hang into a failure. */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "wine/osync.h"

static void *base;
static uint32_t next_chunk = 1;
static uint32_t next_cell, cells_left;
static int failures, checks;
static const char *current_test;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("  FAILED %s:%d (%s): %s\n", __FILE__, __LINE__, current_test, #cond); } } while (0)

static uint32_t tid_of( int i ) { return 4 * (uint32_t)(i + 1); }

/* ---- the server's part -------------------------------------------------------------------------------------- */

static pthread_mutex_t alloc_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t alloc_chunk(void)
{
    uint32_t chunk;
    pthread_mutex_lock( &alloc_mutex );
    chunk = next_chunk++;
    pthread_mutex_unlock( &alloc_mutex );
    assert( chunk < OSYNC_CELLS / OSYNC_BLOCK_CELLS );
    return chunk;
}

static uint32_t new_cell(void)
{
    uint32_t cell;
    pthread_mutex_lock( &alloc_mutex );
    if (!cells_left)
    {
        next_cell = next_chunk++ * OSYNC_BLOCK_CELLS;
        cells_left = OSYNC_BLOCK_CELLS;
    }
    cell = next_cell++;
    cells_left--;
    pthread_mutex_unlock( &alloc_mutex );
    return cell;
}

/* returns the object's name */
static uint32_t create( int type, uint32_t state, uint32_t max, uint32_t owner )
{
    uint32_t cell = new_cell();
    struct osync_obj *obj = osync_obj( base, cell );
    memset( obj, 0, sizeof(*obj) );
    obj->gen = 1;
    obj->type = type;
    obj->state = state;
    obj->max = max;
    osync_set_owner( obj, owner );
    return 1u << OSYNC_GEN_SHIFT | cell;
}

static uint32_t cell_of( uint32_t name ) { return name & OSYNC_CELL_MASK; }

static uint32_t new_block( uint32_t tid )
{
    uint32_t cell = alloc_chunk() * OSYNC_BLOCK_CELLS;
    struct osync_block *block = osync_block( base, cell );
    memset( block, 0, OSYNC_BLOCK_CELLS * OSYNC_CELL_SIZE );
    block->tid = tid;
    return cell;
}

/* what wineserver does for a dead thread: cancel its wait, unlink its entries, free its locks */
static void server_cleanup_thread( uint32_t block_cell, uint32_t tid, const uint32_t *names, int count )
{
    struct osync_block *block = osync_block( base, block_cell );
    int i;

    for (i = 0; i < count; i++)
    {
        struct osync_obj *obj = osync_obj( base, cell_of( names[i] ) );
        uint32_t cur = osync_load( &obj->lock );
        if (cur && (cur & ~OSYNC_LOCK_CONTENDED) == tid && osync_cas( &obj->lock, cur, 0 ))
            os_sync_wake_by_address_all( &obj->lock, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED );
    }
    osync_cas( &block->status, OSYNC_WAITING, OSYNC_CANCELLED );
    osync_pass_on( base, block, osync_load( &block->status ), OSYNC_SERVER_TID );
    for (i = 0; i < OSYNC_MAX_ENTRIES; i++)
    {
        struct osync_entry *entry = &block->entries[i];
        struct osync_obj *obj;
        if (!(entry->flags & OSYNC_ENTRY_LINKED)) continue;
        obj = osync_obj( base, entry->obj );
        osync_lock( &obj->lock, OSYNC_SERVER_TID );
        osync_unlink( base, obj, entry );
        osync_unlock( &obj->lock );
    }
    block->status = OSYNC_IDLE;
}

static void server_abandon( uint32_t name, uint32_t tid )
{
    struct osync_obj *obj = osync_obj( base, cell_of( name ) );
    struct osync_wakes wakes = { 0 };
    int all = osync_lock_for_signal( base, obj, OSYNC_SERVER_TID );
    if (osync_owner( obj ) == tid)
    {
        osync_set_owner( obj, 0 );
        obj->state = 0;
        obj->abandoned = 1;
        osync_signaled( base, cell_of( name ), obj, OSYNC_SERVER_TID, &wakes );
    }
    osync_unlock_for_signal( base, obj, all );
    osync_flush_wakes( &wakes );
}

/* ---- helpers ------------------------------------------------------------------------------------------------ */

static uint64_t ms( uint64_t n ) { return n * 1000000; }

static uint32_t wait1( uint32_t block, uint32_t tid, uint32_t name, uint64_t timeout_ns )
{
    uint32_t cell = cell_of( name );
    return osync_wait_any( base, block, tid, 1, &cell, 0, timeout_ns == OSYNC_INFINITE ? OSYNC_INFINITE
                           : timeout_ns ? osync_now() + timeout_ns : 0 );
}

static uint32_t waitn( uint32_t block, uint32_t tid, int all, int count, const uint32_t *names, uint32_t alert,
                       uint64_t timeout_ns )
{
    uint32_t cells[OSYNC_MAX_ENTRIES], deadline_cells;
    uint64_t deadline = timeout_ns == OSYNC_INFINITE ? OSYNC_INFINITE : timeout_ns ? osync_now() + timeout_ns : 0;
    int i, invalid;

    for (i = 0; i < count; i++) cells[i] = cell_of( names[i] );
    deadline_cells = alert ? cell_of( alert ) : 0;
    if (!all) return osync_wait_any( base, block, tid, count, cells, deadline_cells, deadline );
    return osync_wait_all( base, block, tid, count, cells, deadline_cells, deadline, &invalid ) | (invalid ? 0x80000000u : 0);
}

static int linked_count( uint32_t name )
{
    struct osync_obj *obj = osync_obj( base, cell_of( name ) );
    int n = 0;
    uint32_t id;
    osync_lock( &obj->lock, OSYNC_SERVER_TID );
    for (id = obj->head; id; id = osync_entry( base, id )->next) n++;
    osync_unlock( &obj->lock );
    return n;
}

static void wait_linked( uint32_t name, int n )
{
    int i;
    for (i = 0; i < 20000 && linked_count( name ) < n; i++) usleep( 100 );
}

static int is_ok( uint32_t status, uint32_t slot ) { return status == (OSYNC_SIGNALED | slot); }

static uint32_t state_of( uint32_t name ) { return osync_obj( base, cell_of( name ) )->state; }

/* ---- semantics ---------------------------------------------------------------------------------------------- */

static void test_events(void)
{
    uint32_t b = new_block( tid_of( 0 ) ), me = tid_of( 0 ), prev = 99;
    uint32_t manual = create( OSYNC_EVENT_MANUAL, 0, 0, 0 ), autoev = create( OSYNC_EVENT_AUTO, 1, 0, 0 );
    uint64_t t;

    current_test = "events";
    CHECK( wait1( b, me, manual, 0 ) == OSYNC_CANCELLED );
    CHECK( osync_event_op( base, manual, 0, me, &prev ) == OSYNC_OK && prev == 0 );
    CHECK( is_ok( wait1( b, me, manual, 0 ), 0 ) );
    CHECK( is_ok( wait1( b, me, manual, 0 ), 0 ) );         /* manual stays set */
    CHECK( osync_event_op( base, manual, 1, me, &prev ) == OSYNC_OK && prev == 1 );
    CHECK( wait1( b, me, manual, 0 ) == OSYNC_CANCELLED );
    CHECK( is_ok( wait1( b, me, autoev, 0 ), 0 ) );
    CHECK( wait1( b, me, autoev, 0 ) == OSYNC_CANCELLED );   /* auto resets */
    CHECK( osync_event_op( base, autoev, 0, me, &prev ) == OSYNC_OK && prev == 0 );
    CHECK( osync_event_op( base, autoev, 0, me, &prev ) == OSYNC_OK && prev == 1 );
    CHECK( osync_event_op( base, autoev, 2, me, &prev ) == OSYNC_OK && prev == 1 );  /* pulse, nobody waiting */
    CHECK( state_of( autoev ) == 0 );

    t = osync_now();
    CHECK( wait1( b, me, manual, ms( 50 ) ) == OSYNC_CANCELLED );
    t = osync_now() - t;
    CHECK( t >= ms( 49 ) && t < ms( 500 ) );
    CHECK( osync_load( &osync_block( base, b )->status ) == OSYNC_IDLE );
    CHECK( linked_count( manual ) == 0 );
}

static void test_semaphores(void)
{
    uint32_t b = new_block( tid_of( 0 ) ), me = tid_of( 0 ), prev = 99;
    uint32_t sem = create( OSYNC_SEMAPHORE, 1, 3, 0 );
    struct osync_obj copy;

    current_test = "semaphores";
    CHECK( osync_release_semaphore( base, sem, 2, me, &prev ) == OSYNC_OK && prev == 1 );
    CHECK( osync_release_semaphore( base, sem, 1, me, &prev ) == OSYNC_LIMIT );
    CHECK( state_of( sem ) == 3 );
    CHECK( osync_read( base, sem, me, &copy ) == OSYNC_OK && copy.state == 3 && copy.max == 3 );
    CHECK( is_ok( wait1( b, me, sem, 0 ), 0 ) );
    CHECK( is_ok( wait1( b, me, sem, 0 ), 0 ) );
    CHECK( is_ok( wait1( b, me, sem, 0 ), 0 ) );
    CHECK( wait1( b, me, sem, 0 ) == OSYNC_CANCELLED );
    CHECK( osync_release_semaphore( base, sem, 0xffffffffu, me, &prev ) == OSYNC_LIMIT );  /* no wraparound */
}

static void test_mutexes(void)
{
    uint32_t b = new_block( tid_of( 0 ) ), b2 = new_block( tid_of( 1 ) ), me = tid_of( 0 ), other = tid_of( 1 ), prev;
    uint32_t m = create( OSYNC_MUTEX, 0, 0, 0 );

    current_test = "mutexes";
    CHECK( is_ok( wait1( b, me, m, 0 ), 0 ) );
    CHECK( is_ok( wait1( b, me, m, 0 ), 0 ) );               /* recursion */
    CHECK( state_of( m ) == 2 );
    CHECK( wait1( b2, other, m, 0 ) == OSYNC_CANCELLED );
    CHECK( osync_release_mutex( base, m, other, &prev ) == OSYNC_NOT_OWNED );
    CHECK( osync_release_mutex( base, m, me, &prev ) == OSYNC_OK && prev == 2 );
    CHECK( osync_release_mutex( base, m, me, &prev ) == OSYNC_OK && prev == 1 );
    CHECK( osync_release_mutex( base, m, me, &prev ) == OSYNC_NOT_OWNED );
    CHECK( osync_owner( osync_obj( base, cell_of( m ) ) ) == 0 );

    /* abandoned by a dead owner: the next waiter gets it, flagged once */
    CHECK( is_ok( wait1( b, me, m, 0 ), 0 ) );
    server_abandon( m, me );
    CHECK( osync_obj( base, cell_of( m ) )->abandoned == 1 );
    CHECK( wait1( b2, other, m, 0 ) == (OSYNC_SIGNALED | OSYNC_ABANDONED) );
    CHECK( osync_obj( base, cell_of( m ) )->abandoned == 0 );
    CHECK( osync_release_mutex( base, m, other, &prev ) == OSYNC_OK );
    CHECK( is_ok( wait1( b2, other, m, 0 ), 0 ) );
    CHECK( osync_release_mutex( base, m, other, &prev ) == OSYNC_OK );
}

static void test_stale_names(void)
{
    uint32_t me = tid_of( 0 ), prev;
    uint32_t ev = create( OSYNC_EVENT_MANUAL, 0, 0, 0 ), stale = ev & OSYNC_CELL_MASK, dead;
    struct osync_obj copy;

    current_test = "stale names";
    stale |= 2u << OSYNC_GEN_SHIFT;                          /* another generation of the same cell */
    CHECK( osync_event_op( base, stale, 0, me, &prev ) == OSYNC_BAD_HANDLE );
    CHECK( osync_read( base, stale, me, &copy ) == OSYNC_BAD_HANDLE );
    CHECK( osync_release_semaphore( base, stale, 1, me, &prev ) == OSYNC_BAD_HANDLE );
    CHECK( osync_release_mutex( base, stale, me, &prev ) == OSYNC_BAD_HANDLE );
    CHECK( osync_event_op( base, 0, 0, me, &prev ) == OSYNC_BAD_HANDLE );  /* cell 0 is the header */
    dead = create( OSYNC_EVENT_MANUAL, 0, 0, 0 );
    osync_obj( base, cell_of( dead ) )->type = OSYNC_FREE;  /* freed by the server */
    CHECK( osync_event_op( base, dead, 0, me, &prev ) == OSYNC_BAD_HANDLE );
}

static void test_wait_any_order(void)
{
    uint32_t b = new_block( tid_of( 0 ) ), me = tid_of( 0 ), names[4], i;

    current_test = "wait any order";
    for (i = 0; i < 4; i++) names[i] = create( OSYNC_EVENT_AUTO, 0, 0, 0 );
    osync_event_op( base, names[3], 0, me, NULL );
    osync_event_op( base, names[1], 0, me, NULL );
    CHECK( is_ok( waitn( b, me, 0, 4, names, 0, 0 ), 1 ) ); /* lowest signalled index first */
    CHECK( is_ok( waitn( b, me, 0, 4, names, 0, 0 ), 3 ) );
    CHECK( waitn( b, me, 0, 4, names, 0, 0 ) == OSYNC_CANCELLED );
}

static void test_wait_all_semantics(void)
{
    uint32_t b = new_block( tid_of( 0 ) ), me = tid_of( 0 ), names[3], dup[2], alert;

    current_test = "wait all";
    names[0] = create( OSYNC_EVENT_AUTO, 1, 0, 0 );
    names[1] = create( OSYNC_SEMAPHORE, 0, 5, 0 );
    names[2] = create( OSYNC_MUTEX, 0, 0, 0 );
    CHECK( waitn( b, me, 1, 3, names, 0, 0 ) == OSYNC_CANCELLED );
    CHECK( state_of( names[0] ) == 1 );                      /* nothing taken when not all are available */
    osync_release_semaphore( base, names[1], 1, me, NULL );
    CHECK( waitn( b, me, 1, 3, names, 0, 0 ) == OSYNC_SIGNALED );
    CHECK( state_of( names[0] ) == 0 && state_of( names[1] ) == 0 && osync_owner( osync_obj( base, cell_of( names[2] ) ) ) == me );
    osync_release_mutex( base, names[2], me, NULL );

    dup[0] = dup[1] = names[1];
    CHECK( waitn( b, me, 1, 2, dup, 0, 0 ) & 0x80000000u );  /* the same object twice is refused */

    alert = create( OSYNC_EVENT_MANUAL, 1, 0, 0 );
    CHECK( is_ok( waitn( b, me, 1, 3, names, alert, ms( 100 ) ), 3 ) ); /* the APC alert ends a wait all */
}

struct waiter_args
{
    uint32_t block, tid, name, status, all, count, names[8], alert;
    uint64_t timeout;
};

static void *waiter_thread( void *arg )
{
    struct waiter_args *a = arg;
    a->status = a->count ? waitn( a->block, a->tid, a->all, a->count, a->names, a->alert, a->timeout )
                         : wait1( a->block, a->tid, a->name, a->timeout );
    return NULL;
}

static void start_waiter( pthread_t *thread, struct waiter_args *a, int i, uint32_t name, uint64_t timeout )
{
    memset( a, 0, sizeof(*a) );
    a->tid = tid_of( i );
    a->block = new_block( a->tid );
    a->name = name;
    a->timeout = timeout;
    pthread_create( thread, NULL, waiter_thread, a );
}

static void test_handoff(void)
{
    struct waiter_args a[4];
    pthread_t t[4];
    uint32_t me = tid_of( 10 ), ev = create( OSYNC_EVENT_AUTO, 0, 0, 0 ), sem = create( OSYNC_SEMAPHORE, 0, 10, 0 );
    uint32_t m = create( OSYNC_MUTEX, 1, 0, me ), i, woken;

    current_test = "hand-off";
    /* auto event: exactly one of several waiters, and the event stays reset */
    for (i = 0; i < 3; i++) start_waiter( &t[i], &a[i], i, ev, ms( 500 ) );
    wait_linked( ev, 3 );
    osync_event_op( base, ev, 0, me, NULL );
    for (i = 0, woken = 0; i < 3; i++) { pthread_join( t[i], NULL ); woken += is_ok( a[i].status, 0 ); }
    CHECK( woken == 1 );
    CHECK( state_of( ev ) == 0 );
    CHECK( linked_count( ev ) == 0 );

    /* semaphore: a release of 2 wakes two of three, and the count is used up */
    for (i = 0; i < 3; i++) start_waiter( &t[i], &a[i], i, sem, ms( 500 ) );
    wait_linked( sem, 3 );
    osync_release_semaphore( base, sem, 2, me, NULL );
    for (i = 0, woken = 0; i < 3; i++) { pthread_join( t[i], NULL ); woken += is_ok( a[i].status, 0 ); }
    CHECK( woken == 2 );
    CHECK( state_of( sem ) == 0 );

    /* mutex: handed over on release, the new owner is the waiter */
    start_waiter( &t[0], &a[0], 0, m, ms( 2000 ) );
    wait_linked( m, 1 );
    osync_release_mutex( base, m, me, NULL );
    pthread_join( t[0], NULL );
    CHECK( is_ok( a[0].status, 0 ) );
    CHECK( osync_owner( osync_obj( base, cell_of( m ) ) ) == a[0].tid );

    /* mutex abandoned while someone waits */
    start_waiter( &t[1], &a[1], 1, m, ms( 2000 ) );
    wait_linked( m, 1 );
    server_abandon( m, a[0].tid );
    pthread_join( t[1], NULL );
    CHECK( a[1].status == (OSYNC_SIGNALED | OSYNC_ABANDONED) );
    CHECK( osync_owner( osync_obj( base, cell_of( m ) ) ) == a[1].tid );
}

static void test_pulse(void)
{
    struct waiter_args a[5];
    pthread_t t[5];
    uint32_t me = tid_of( 10 ), manual = create( OSYNC_EVENT_MANUAL, 0, 0, 0 ), autoev = create( OSYNC_EVENT_AUTO, 0, 0, 0 );
    uint32_t i, woken;

    current_test = "PulseEvent";
    for (i = 0; i < 5; i++) start_waiter( &t[i], &a[i], i, manual, ms( 1000 ) );
    wait_linked( manual, 5 );
    osync_event_op( base, manual, 2, me, NULL );
    for (i = 0, woken = 0; i < 5; i++) { pthread_join( t[i], NULL ); woken += is_ok( a[i].status, 0 ); }
    CHECK( woken == 5 );                                     /* every waiter, none missed */
    CHECK( state_of( manual ) == 0 );

    for (i = 0; i < 5; i++) start_waiter( &t[i], &a[i], i, autoev, ms( 300 ) );
    wait_linked( autoev, 5 );
    osync_event_op( base, autoev, 2, me, NULL );
    for (i = 0, woken = 0; i < 5; i++) { pthread_join( t[i], NULL ); woken += is_ok( a[i].status, 0 ); }
    CHECK( woken == 1 );
    CHECK( state_of( autoev ) == 0 );
}

static void test_alert(void)
{
    struct waiter_args a;
    pthread_t t;
    uint32_t names[2], alert = create( OSYNC_EVENT_MANUAL, 0, 0, 0 );

    current_test = "alert";
    names[0] = create( OSYNC_EVENT_AUTO, 0, 0, 0 );
    names[1] = create( OSYNC_SEMAPHORE, 0, 1, 0 );
    memset( &a, 0, sizeof(a) );
    a.tid = tid_of( 0 );
    a.block = new_block( a.tid );
    a.count = 2;
    a.names[0] = names[0];
    a.names[1] = names[1];
    a.alert = alert;
    a.timeout = ms( 2000 );
    pthread_create( &t, NULL, waiter_thread, &a );
    wait_linked( alert, 1 );
    osync_event_op( base, alert, 0, OSYNC_SERVER_TID, NULL );
    pthread_join( t, NULL );
    CHECK( is_ok( a.status, 2 ) );
    CHECK( linked_count( names[0] ) == 0 && linked_count( names[1] ) == 0 && linked_count( alert ) == 0 );
}

static void test_wait_all_blocking(void)
{
    struct waiter_args a;
    pthread_t t;
    uint32_t me = tid_of( 10 ), e0 = create( OSYNC_EVENT_AUTO, 0, 0, 0 ), e1 = create( OSYNC_EVENT_AUTO, 0, 0, 0 );

    current_test = "wait all blocking";
    memset( &a, 0, sizeof(a) );
    a.tid = tid_of( 0 );
    a.block = new_block( a.tid );
    a.all = 1;
    a.count = 2;
    a.names[0] = e0;
    a.names[1] = e1;
    a.timeout = ms( 2000 );
    pthread_create( &t, NULL, waiter_thread, &a );
    wait_linked( e0, 1 );
    osync_event_op( base, e0, 0, me, NULL );
    usleep( 20000 );
    CHECK( state_of( e0 ) == 1 );                            /* not taken alone */
    osync_event_op( base, e1, 0, me, NULL );
    pthread_join( t, NULL );
    CHECK( a.status == OSYNC_SIGNALED );
    CHECK( state_of( e0 ) == 0 && state_of( e1 ) == 0 );
    CHECK( osync_obj( base, cell_of( e0 ) )->all_waiters == 0 && osync_obj( base, cell_of( e1 ) )->all_waiters == 0 );
}

/* ---- stress ------------------------------------------------------------------------------------------------- */

#define STRESS_THREADS 8
#define STRESS_SEMS 4

struct stress
{
    uint32_t sems[STRESS_SEMS], mutex, ev;
    volatile long acquired[STRESS_SEMS], released[STRESS_SEMS], in_mutex, mutex_violations, stop;
    long iterations;
};

struct stress_arg { struct stress *s; int index; unsigned int seed; uint32_t block; };

static void *stress_thread( void *p )
{
    struct stress_arg *arg = p;
    struct stress *s = arg->s;
    uint32_t tid = tid_of( arg->index ), block = arg->block ? arg->block : new_block( tid ), cells[STRESS_SEMS], i;
    unsigned int seed = arg->seed;
    long n;

    for (i = 0; i < STRESS_SEMS; i++) cells[i] = cell_of( s->sems[i] );
    for (n = 0; n < s->iterations; n++)
    {
        int op = rand_r( &seed ) % 6, k = rand_r( &seed ) % STRESS_SEMS, j;
        uint64_t timeout = (rand_r( &seed ) % 4) ? osync_now() + (rand_r( &seed ) % 200) * 1000 : 0;
        uint32_t status, slot;
        int invalid;

        switch (op)
        {
        case 0:  /* release one */
            if (!osync_release_semaphore( base, s->sems[k], 1, tid, NULL )) __atomic_add_fetch( &s->released[k], 1, __ATOMIC_SEQ_CST );
            break;
        case 1: case 2:  /* wait any, with a short timeout racing the releases */
            status = osync_wait_any( base, block, tid, STRESS_SEMS, cells, 0, timeout );
            if (status != OSYNC_CANCELLED)
            {
                slot = status & OSYNC_SLOT_MASK;
                __atomic_add_fetch( &s->acquired[slot], 1, __ATOMIC_SEQ_CST );
            }
            break;
        case 3:  /* wait all on two */
        {
            uint32_t two[2] = { cells[k], cells[(k + 1) % STRESS_SEMS] };
            status = osync_wait_all( base, block, tid, 2, two, 0, timeout, &invalid );
            if (status == OSYNC_SIGNALED)
                for (j = 0; j < 2; j++) __atomic_add_fetch( &s->acquired[(k + j) % STRESS_SEMS], 1, __ATOMIC_SEQ_CST );
            break;
        }
        case 4:  /* mutex exclusion */
        {
            uint32_t m = cell_of( s->mutex );
            status = osync_wait_any( base, block, tid, 1, &m, 0, osync_now() + ms( 50 ) );
            if (status == OSYNC_SIGNALED)
            {
                if (__atomic_add_fetch( &s->in_mutex, 1, __ATOMIC_SEQ_CST ) != 1) __atomic_add_fetch( &s->mutex_violations, 1, __ATOMIC_SEQ_CST );
                __atomic_sub_fetch( &s->in_mutex, 1, __ATOMIC_SEQ_CST );
                if (osync_release_mutex( base, s->mutex, tid, NULL )) __atomic_add_fetch( &s->mutex_violations, 1, __ATOMIC_SEQ_CST );
            }
            break;
        }
        case 5:  /* pulse and set/reset traffic on an event */
            osync_event_op( base, s->ev, rand_r( &seed ) % 3, tid, NULL );
            break;
        }
    }
    return NULL;
}

static void test_stress( long iterations )
{
    struct stress s;
    struct stress_arg args[STRESS_THREADS];
    pthread_t t[STRESS_THREADS];
    int i;

    current_test = "stress";
    memset( &s, 0, sizeof(s) );
    for (i = 0; i < STRESS_SEMS; i++) s.sems[i] = create( OSYNC_SEMAPHORE, 0, 1000000, 0 );
    s.mutex = create( OSYNC_MUTEX, 0, 0, 0 );
    s.ev = create( OSYNC_EVENT_MANUAL, 0, 0, 0 );
    s.iterations = iterations;
    for (i = 0; i < STRESS_THREADS; i++)
    {
        args[i].s = &s;
        args[i].index = i;
        args[i].seed = 1234 + i;
        args[i].block = 0;
        pthread_create( &t[i], NULL, stress_thread, &args[i] );
    }
    for (i = 0; i < STRESS_THREADS; i++) pthread_join( t[i], NULL );

    /* every unit released was either acquired or is still in the semaphore */
    for (i = 0; i < STRESS_SEMS; i++)
    {
        CHECK( s.released[i] == s.acquired[i] + (long)state_of( s.sems[i] ) );
        CHECK( linked_count( s.sems[i] ) == 0 );
        CHECK( osync_obj( base, cell_of( s.sems[i] ) )->all_waiters == 0 );
    }
    CHECK( s.mutex_violations == 0 );
    CHECK( osync_owner( osync_obj( base, cell_of( s.mutex ) ) ) == 0 );
    CHECK( linked_count( s.mutex ) == 0 );
    printf( "  stress: %d threads x %ld operations\n", STRESS_THREADS, iterations );
}

/* ---- mutex contention ---------------------------------------------------------------------------------------- */

#define CONTENTION_THREADS 8
static struct { uint32_t mutex; long per_thread; volatile long counter, inside, violations; } cont;

static void *contention_thread( void *p )
{
    int index = (int)(long)p;
    uint32_t tid = tid_of( 50 + index ), block = new_block( tid ), cell = cell_of( cont.mutex );
    long i;

    for (i = 0; i < cont.per_thread; i++)
    {
        if (osync_wait_any( base, block, tid, 1, &cell, 0, OSYNC_INFINITE ) != OSYNC_SIGNALED)
            __atomic_add_fetch( &cont.violations, 1, __ATOMIC_SEQ_CST );
        if (__atomic_add_fetch( &cont.inside, 1, __ATOMIC_SEQ_CST ) != 1) __atomic_add_fetch( &cont.violations, 1, __ATOMIC_SEQ_CST );
        cont.counter++;  /* plain: the mutex must protect it */
        __atomic_sub_fetch( &cont.inside, 1, __ATOMIC_SEQ_CST );
        if (osync_release_mutex( base, cont.mutex, tid, NULL )) __atomic_add_fetch( &cont.violations, 1, __ATOMIC_SEQ_CST );
    }
    return NULL;
}

static void test_mutex_contention( long per_thread )
{
    pthread_t t[CONTENTION_THREADS];
    uint64_t start;
    long i;

    current_test = "mutex contention";
    cont.mutex = create( OSYNC_MUTEX, 0, 0, 0 );
    cont.per_thread = per_thread;
    cont.counter = cont.inside = cont.violations = 0;
    start = osync_now();
    for (i = 0; i < CONTENTION_THREADS; i++) pthread_create( &t[i], NULL, contention_thread, (void *)i );
    for (i = 0; i < CONTENTION_THREADS; i++) pthread_join( t[i], NULL );
    CHECK( cont.violations == 0 );
    CHECK( cont.counter == CONTENTION_THREADS * per_thread );
    CHECK( osync_owner( osync_obj( base, cell_of( cont.mutex ) ) ) == 0 && linked_count( cont.mutex ) == 0 );
    printf( "  mutex contention: %d threads x %ld in %.0f ms\n", CONTENTION_THREADS, per_thread, (osync_now() - start) / 1e6 );
}

/* The rule itself, with hand-made waiters: of two queued for a mutex, the one that already lost the retry race
 * twice gets the next release handed over, ahead of an awake one that hasn't. */
static void test_starvation_rule(void)
{
    uint32_t owner = tid_of( 72 ), m = create( OSYNC_MUTEX, 1, 0, owner );
    struct osync_obj *obj = osync_obj( base, cell_of( m ) );
    uint32_t b1 = new_block( tid_of( 73 ) ), b2 = new_block( tid_of( 74 ) ), i;
    struct osync_block *awake = osync_block( base, b1 ), *starving = osync_block( base, b2 );

    current_test = "starvation rule";
    for (i = 0; i < 2; i++)
    {
        struct osync_block *b = i ? starving : awake;
        b->count = 1;
        b->entries[0].obj = cell_of( m );
        b->entries[0].slot = 0;
        b->entries[0].flags = 0;
        b->sleeping = i;                 /* the starving one sleeps */
        b->lost = i ? 2 : 0;             /* two lost races: enough (OSYNC_MAX_LOST is 2) */
        b->status = OSYNC_WAITING;
        osync_lock( &obj->lock, owner );
        osync_link( base, obj, &b->entries[0] );
        osync_unlock( &obj->lock );
    }
    CHECK( osync_release_mutex( base, m, owner, NULL ) == OSYNC_OK );
    CHECK( starving->status == OSYNC_SIGNALED );            /* handed over */
    CHECK( osync_owner( obj ) == tid_of( 74 ) && obj->state == 1 );
    CHECK( awake->status == OSYNC_WAITING );                 /* not even woken */
    CHECK( linked_count( m ) == 1 );

    /* without a starving waiter, the awake one is woken to retry and the mutex stays free for whoever is quicker */
    starving->status = OSYNC_IDLE;
    CHECK( osync_release_mutex( base, m, tid_of( 74 ), NULL ) == OSYNC_OK );
    CHECK( awake->status == (OSYNC_RETRY | 0) );
    CHECK( osync_owner( obj ) == 0 );
    CHECK( linked_count( m ) == 0 );
    awake->status = OSYNC_IDLE;
}

/* A thread that re-takes a mutex in a tight loop must not starve one that waits for it: after losing the race a
 * few times, the waiter gets it handed over. */
#define HOT_ROUNDS 20000
static struct { uint32_t mutex; volatile long hot_done, hot_at_waiter, waiter_started; } starve;

static void *hot_thread( void *p )
{
    uint32_t tid = tid_of( 70 ), block = new_block( tid ), cell = cell_of( starve.mutex );
    long i;

    for (i = 0; i < HOT_ROUNDS; i++)
    {
        osync_wait_any( base, block, tid, 1, &cell, 0, OSYNC_INFINITE );
        if (i == 100) __atomic_store_n( &starve.waiter_started, 1, __ATOMIC_SEQ_CST );  /* let the waiter in */
        osync_release_mutex( base, starve.mutex, tid, NULL );
        __atomic_store_n( &starve.hot_done, i + 1, __ATOMIC_SEQ_CST );
    }
    return NULL;
}

static void test_starvation(void)
{
    uint32_t tid = tid_of( 71 ), block = new_block( tid ), cell;
    pthread_t t;
    uint32_t status;

    current_test = "starvation";
    memset( (void *)&starve, 0, sizeof(starve) );
    starve.mutex = create( OSYNC_MUTEX, 0, 0, 0 );
    cell = cell_of( starve.mutex );
    pthread_create( &t, NULL, hot_thread, NULL );
    while (!__atomic_load_n( &starve.waiter_started, __ATOMIC_SEQ_CST )) usleep( 10 );
    status = osync_wait_any( base, block, tid, 1, &cell, 0, osync_now() + ms( 10000 ) );
    starve.hot_at_waiter = __atomic_load_n( &starve.hot_done, __ATOMIC_SEQ_CST );
    if (status == OSYNC_SIGNALED) osync_release_mutex( base, starve.mutex, tid, NULL );
    pthread_join( t, NULL );
    CHECK( status == OSYNC_SIGNALED );
    CHECK( starve.hot_at_waiter < HOT_ROUNDS / 2 );
    printf( "  starvation: the waiter got the mutex after %ld of %d hot rounds\n", starve.hot_at_waiter, HOT_ROUNDS );
}

/* A waiter woken to retry a mutex dies before taking it (or after: then it owned it): the next one must still get
 * it, through the server's cleanup. */
static void test_retry_death(void)
{
    int round, ok = 1;

    current_test = "retry and death";
    for (round = 0; round < 60 && ok; round++)
    {
        uint32_t owner = tid_of( 60 ), m = create( OSYNC_MUTEX, 1, 0, owner );
        uint32_t child_tid = tid_of( 61 ), child_block = new_block( child_tid );
        struct waiter_args b;
        pthread_t t;
        pid_t pid;
        int status;

        if (!(pid = fork()))
        {
            alarm( 60 );
            wait1( child_block, child_tid, m, OSYNC_INFINITE );
            pause();  /* holds it, if it got it */
            _exit( 0 );
        }
        wait_linked( m, 1 );
        start_waiter( &t, &b, 62, m, ms( 5000 ) );
        wait_linked( m, 2 );
        osync_release_mutex( base, m, owner, NULL );   /* the child is first: it gets the retry */
        {
            static const useconds_t delays[] = { 0, 20, 200, 1000 };  /* hit the windows before and after it takes it */
            if (delays[round % 4]) usleep( delays[round % 4] );
        }
        kill( pid, SIGKILL );
        waitpid( pid, &status, 0 );
        server_cleanup_thread( child_block, child_tid, &m, 1 );
        server_abandon( m, child_tid );
        pthread_join( t, NULL );
        ok = (b.status & ~OSYNC_ABANDONED) == OSYNC_SIGNALED && osync_owner( osync_obj( base, cell_of( m ) ) ) == b.tid;
        if (!ok) printf( "  round %d: status %#x owner %#x (waiter %#x, child %#x) lock %#x linked %d\n", round, b.status,
                         osync_owner( osync_obj( base, cell_of( m ) ) ), b.tid, child_tid,
                         osync_obj( base, cell_of( m ) )->lock, linked_count( m ) );
    }
    CHECK( ok );
}

/* ---- processes ---------------------------------------------------------------------------------------------- */

static void test_processes(void)
{
    uint32_t ping = create( OSYNC_EVENT_AUTO, 0, 0, 0 ), pong = create( OSYNC_EVENT_AUTO, 0, 0, 0 );
    uint32_t sem = create( OSYNC_SEMAPHORE, 0, 10, 0 ), m = create( OSYNC_MUTEX, 0, 0, 0 );
    uint32_t child_tid = tid_of( 20 ), child_block = new_block( child_tid ), me = tid_of( 0 ), b = new_block( me );
    uint32_t parent_lock_names[2];
    int status, i, rounds = 20000, ok = 1;
    uint64_t t;
    pid_t pid;

    current_test = "processes";
    /* ping-pong between two processes */
    if (!(pid = fork()))
    {
        alarm( 120 );
        for (i = 0; i < rounds; i++)
        {
            if (!is_ok( wait1( child_block, child_tid, ping, ms( 5000 ) ), 0 )) _exit( 1 );
            osync_event_op( base, pong, 0, child_tid, NULL );
        }
        _exit( 0 );
    }
    t = osync_now();
    for (i = 0; i < rounds && ok; i++)
    {
        osync_event_op( base, ping, 0, me, NULL );
        ok = is_ok( wait1( b, me, pong, ms( 5000 ) ), 0 );
    }
    t = osync_now() - t;
    waitpid( pid, &status, 0 );
    CHECK( ok && WIFEXITED( status ) && WEXITSTATUS( status ) == 0 );
    printf( "  processes: %d round trips, %.2f us each\n", rounds, t / 1000.0 / rounds );

    /* a process killed while waiting: after the server's cleanup the count goes to the living */
    if (!(pid = fork()))
    {
        alarm( 120 );
        wait1( child_block, child_tid, sem, OSYNC_INFINITE );
        _exit( 0 );
    }
    wait_linked( sem, 1 );
    kill( pid, SIGKILL );
    waitpid( pid, &status, 0 );
    server_cleanup_thread( child_block, child_tid, &sem, 1 );
    CHECK( linked_count( sem ) == 0 );
    osync_release_semaphore( base, sem, 1, me, NULL );
    CHECK( is_ok( wait1( b, me, sem, 0 ), 0 ) );

    /* a process killed holding an object's lock and owning a mutex */
    if (!(pid = fork()))
    {
        alarm( 120 );
        wait1( child_block, child_tid, m, 0 );
        osync_lock( &osync_obj( base, cell_of( sem ) )->lock, child_tid );
        pause();
        _exit( 0 );
    }
    for (i = 0; i < 20000 && osync_load( &osync_obj( base, cell_of( sem ) )->lock ) == 0; i++) usleep( 100 );
    kill( pid, SIGKILL );
    waitpid( pid, &status, 0 );
    parent_lock_names[0] = sem;
    parent_lock_names[1] = m;
    server_cleanup_thread( child_block, child_tid, parent_lock_names, 2 );
    server_abandon( m, child_tid );
    CHECK( osync_release_semaphore( base, sem, 1, me, NULL ) == OSYNC_OK );  /* lock free again */
    CHECK( wait1( b, me, m, 0 ) == (OSYNC_SIGNALED | OSYNC_ABANDONED) );
    osync_release_mutex( base, m, me, NULL );
}

/* ---- against a reference model ------------------------------------------------------------------------------ */

/* Windows semantics written plainly, for single-threaded random sequences with zero timeouts; osync must agree on
 * every result and every state. */
#define MODEL_OBJS 12
#define MODEL_TIDS 3

struct model_obj { int type; uint32_t state, max, owner, abandoned; };

static int model_can_take( const struct model_obj *o, uint32_t tid )
{
    if (o->type == OSYNC_MUTEX) return !o->owner || (o->owner == tid && o->state < 0x7fffffff);
    return o->state != 0;
}

static uint32_t model_take( struct model_obj *o, uint32_t tid )
{
    uint32_t ab = 0;
    switch (o->type)
    {
    case OSYNC_EVENT_AUTO: o->state = 0; break;
    case OSYNC_SEMAPHORE: o->state--; break;
    case OSYNC_MUTEX:
        if (o->owner == tid) o->state++; else { o->owner = tid; o->state = 1; }
        if (o->abandoned) ab = OSYNC_ABANDONED;
        o->abandoned = 0;
        break;
    }
    return ab;
}

static void test_model( long steps, unsigned int seed )
{
    struct model_obj model[MODEL_OBJS];
    uint32_t names[MODEL_OBJS], blocks[MODEL_TIDS], i;
    long n, mismatches = 0;
    unsigned int first_seed = seed;

    current_test = "model";
    for (i = 0; i < MODEL_TIDS; i++) blocks[i] = new_block( tid_of( 30 + i ) );
    for (i = 0; i < MODEL_OBJS; i++)
    {
        int type = OSYNC_EVENT_MANUAL + rand_r( &seed ) % 4;
        model[i].type = type;
        model[i].state = type == OSYNC_SEMAPHORE ? rand_r( &seed ) % 3 : type == OSYNC_MUTEX ? 0 : rand_r( &seed ) % 2;
        model[i].max = type == OSYNC_SEMAPHORE ? 1 + rand_r( &seed ) % 5 : 0;
        if (model[i].state > model[i].max && type == OSYNC_SEMAPHORE) model[i].state = model[i].max;
        model[i].owner = model[i].abandoned = 0;
        names[i] = create( type, model[i].state, model[i].max, 0 );
    }

    for (n = 0; n < steps && mismatches < 10; n++)
    {
        int t = rand_r( &seed ) % MODEL_TIDS, k = rand_r( &seed ) % MODEL_OBJS, op = rand_r( &seed ) % 9;
        uint32_t tid = tid_of( 30 + t ), got = 0, want = 0, prev = 0, mprev = 0;
        struct model_obj *o = &model[k];

        switch (op)
        {
        case 0: case 1: case 2:  /* event set / reset / pulse (on any object: a non-event fails the same way) */
            if (o->type != OSYNC_EVENT_MANUAL && o->type != OSYNC_EVENT_AUTO) continue;
            got = osync_event_op( base, names[k], op, tid, &prev );
            mprev = o->state;
            o->state = op == 0 ? 1 : 0;
            want = OSYNC_OK;
            if (got == want && prev != mprev) got = 1000;
            break;
        case 3:  /* semaphore release */
        {
            uint32_t count = 1 + rand_r( &seed ) % 3;
            if (o->type != OSYNC_SEMAPHORE) continue;
            got = osync_release_semaphore( base, names[k], count, tid, &prev );
            if (count > o->max - o->state) want = OSYNC_LIMIT;
            else { mprev = o->state; o->state += count; want = OSYNC_OK; if (got == want && prev != mprev) got = 1000; }
            break;
        }
        case 4:  /* mutex release */
            if (o->type != OSYNC_MUTEX) continue;
            got = osync_release_mutex( base, names[k], tid, &prev );
            if (o->owner != tid) want = OSYNC_NOT_OWNED;
            else
            {
                mprev = o->state;
                if (!--o->state) o->owner = 0;
                want = OSYNC_OK;
                if (got == want && prev != mprev) got = 1000;
            }
            break;
        case 5: case 6:  /* wait any on a random list, possibly with repeats */
        {
            uint32_t count = 1 + rand_r( &seed ) % 5, list[5], cells[5], j;
            for (j = 0; j < count; j++) { list[j] = rand_r( &seed ) % MODEL_OBJS; cells[j] = cell_of( names[list[j]] ); }
            got = osync_wait_any( base, blocks[t], tid, count, cells, 0, 0 );
            want = OSYNC_CANCELLED;
            for (j = 0; j < count; j++)
                if (model_can_take( &model[list[j]], tid ))
                {
                    want = OSYNC_SIGNALED | model_take( &model[list[j]], tid ) | j;
                    break;
                }
            break;
        }
        case 7:  /* wait all on distinct objects */
        {
            uint32_t count = 1 + rand_r( &seed ) % 4, list[4], cells[4], j, l, ok = 1, ab = 0;
            int invalid;
            for (j = 0; j < count; j++)
            {
                do { list[j] = rand_r( &seed ) % MODEL_OBJS; for (l = 0; l < j && list[l] != list[j]; l++); } while (l < j);
                cells[j] = cell_of( names[list[j]] );
            }
            got = count == 1 ? osync_wait_any( base, blocks[t], tid, 1, cells, 0, 0 )
                             : osync_wait_all( base, blocks[t], tid, count, cells, 0, 0, &invalid );
            for (j = 0; j < count; j++) ok &= model_can_take( &model[list[j]], tid );
            if (!ok) want = OSYNC_CANCELLED;
            else
            {
                for (j = 0; j < count; j++) ab |= model_take( &model[list[j]], tid );
                want = OSYNC_SIGNALED | ab;
            }
            break;
        }
        case 8:  /* the server abandons a "dead" thread's mutexes */
            if (o->type != OSYNC_MUTEX || rand_r( &seed ) % 4) continue;
            server_abandon( names[k], tid );
            if (o->owner == tid) { o->owner = 0; o->state = 0; o->abandoned = 1; }
            break;
        }
        if (got != want) { mismatches++; printf( "  step %ld op %d obj %d: got %#x want %#x\n", n, op, k, got, want ); }
        for (i = 0; i < MODEL_OBJS; i++)
        {
            struct osync_obj *obj = osync_obj( base, cell_of( names[i] ) );
            if (obj->state != model[i].state || osync_owner( obj ) != model[i].owner || obj->abandoned != model[i].abandoned)
            {
                mismatches++;
                printf( "  step %ld op %d: object %u state %u/%u owner %u/%u abandoned %u/%u\n", n, op, i,
                        obj->state, model[i].state, osync_owner( obj ), model[i].owner, obj->abandoned, model[i].abandoned );
                break;
            }
        }
    }
    CHECK( mismatches == 0 );
    printf( "  model: %ld steps, seed %u\n", n, first_seed );
}

/* ---- the most a wait can hold ------------------------------------------------------------------------------- */

static void test_max_wait(void)
{
    struct waiter_args a;
    uint32_t names[64], cells[64], alert = create( OSYNC_EVENT_MANUAL, 0, 0, 0 ), me = tid_of( 10 ), i;

    current_test = "64 objects";
    for (i = 0; i < 64; i++) { names[i] = create( OSYNC_EVENT_AUTO, 0, 0, 0 ); cells[i] = cell_of( names[i] ); }
    memset( &a, 0, sizeof(a) );
    a.tid = tid_of( 0 );
    a.block = new_block( a.tid );
    a.status = osync_wait_any( base, a.block, a.tid, 64, cells, cell_of( alert ), 0 );
    CHECK( a.status == OSYNC_CANCELLED );
    osync_event_op( base, names[63], 0, me, NULL );
    CHECK( is_ok( osync_wait_any( base, a.block, a.tid, 64, cells, cell_of( alert ), osync_now() + ms( 100 ) ), 63 ) );
    osync_event_op( base, alert, 0, me, NULL );
    CHECK( is_ok( osync_wait_any( base, a.block, a.tid, 64, cells, cell_of( alert ), osync_now() + ms( 100 ) ), 64 ) );
    for (i = 0; i < 64; i++) CHECK( linked_count( names[i] ) == 0 );
}

/* ---- several processes at once ------------------------------------------------------------------------------ */

static void test_process_stress( long iterations )
{
    struct stress *s = mmap( NULL, sizeof(*s), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0 );
    struct stress_arg arg;
    pid_t pids[4];
    int i, status, ok = 1;

    current_test = "process stress";
    memset( s, 0, sizeof(*s) );
    for (i = 0; i < STRESS_SEMS; i++) s->sems[i] = create( OSYNC_SEMAPHORE, 0, 1000000, 0 );
    s->mutex = create( OSYNC_MUTEX, 0, 0, 0 );
    s->ev = create( OSYNC_EVENT_MANUAL, 0, 0, 0 );
    s->iterations = iterations;
    for (i = 0; i < 4; i++)
    {
        /* the server hands out wait blocks; allocate them here, a child's allocator is a copy */
        uint32_t block = new_block( tid_of( 40 + i ) );
        if (!(pids[i] = fork()))
        {
            alarm( 300 );
            arg.s = s;
            arg.index = 40 + i;
            arg.seed = 777 + i;
            arg.block = block;
            stress_thread( &arg );
            _exit( 0 );
        }
    }
    for (i = 0; i < 4; i++)
    {
        waitpid( pids[i], &status, 0 );
        ok &= WIFEXITED( status ) && WEXITSTATUS( status ) == 0;
    }
    CHECK( ok );
    for (i = 0; i < STRESS_SEMS; i++)
    {
        CHECK( s->released[i] == s->acquired[i] + (long)state_of( s->sems[i] ) );
        CHECK( linked_count( s->sems[i] ) == 0 );
    }
    CHECK( s->mutex_violations == 0 );
    printf( "  process stress: 4 processes x %ld operations\n", iterations );
}

/* ---- main --------------------------------------------------------------------------------------------------- */

static void watchdog( int sig )
{
    static const char msg[] = "FAILED: hang (watchdog)\n";
    write( 1, msg, sizeof(msg) - 1 );
    _exit( 2 );
}

int main( int argc, char **argv )
{
    long iterations = argc > 1 ? atol( argv[1] ) : 200000;

    setvbuf( stdout, NULL, _IONBF, 0 );
    signal( SIGALRM, watchdog );
    alarm( argc > 2 ? atoi( argv[2] ) : 600 );

    base = mmap( NULL, OSYNC_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0 );
    if (base == MAP_FAILED) return 1;
    osync_header( base )->magic = OSYNC_MAGIC;
    osync_header( base )->version = OSYNC_VERSION;

    test_events();
    test_semaphores();
    test_mutexes();
    test_stale_names();
    test_wait_any_order();
    test_wait_all_semantics();
    test_handoff();
    test_pulse();
    test_alert();
    test_wait_all_blocking();
    test_max_wait();
    test_processes();
    test_mutex_contention( iterations / 10 );
    test_starvation_rule();
    test_starvation();
    test_retry_death();
    test_model( iterations * 5, argc > 3 ? (unsigned int)atoi( argv[3] ) : 42 );
    test_stress( iterations );
    test_process_stress( iterations / 2 );

    printf( "%s: %d checks, %d failed\n", failures ? "OSYNC TEST FAILED" : "OSYNC TEST OK", checks, failures );
    return failures ? 1 : 0;
}
