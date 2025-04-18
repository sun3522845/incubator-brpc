// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <algorithm>                         // std::sort
#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include <gtest/gtest.h>
#include "butil/thread_key.h"
#include "butil/time.h"
#include "butil/macros.h"
#include "butil/scoped_lock.h"
#include "butil/logging.h"
#include "bthread/bthread.h"
#include "bthread/unstable.h"
using namespace bthread;
namespace bthread {
DECLARE_uint32(key_table_list_size);
DECLARE_uint32(borrow_from_globle_size);
class KeyTable;
// defined in bthread/key.cpp
extern void return_keytable(bthread_keytable_pool_t*, KeyTable*);
extern KeyTable* borrow_keytable(bthread_keytable_pool_t*);
}  // namespace bthread

int main(int argc, char* argv[]) {
    bthread::FLAGS_key_table_list_size = 20;
    bthread::FLAGS_borrow_from_globle_size = 20;
    testing::InitGoogleTest(&argc, argv);
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
    return RUN_ALL_TESTS();
}

extern "C" {
int bthread_keytable_pool_size(bthread_keytable_pool_t* pool) {
    bthread_keytable_pool_stat_t s;
    if (bthread_keytable_pool_getstat(pool, &s) != 0) {
        return 0;
    }
    return s.nfree;
}
}

namespace {

// Count tls usages.
struct Counters {
    butil::atomic<size_t> ncreate {0};
    butil::atomic<size_t> ndestroy {0};
    butil::atomic<size_t> nenterthread {0};
    butil::atomic<size_t> nleavethread {0};
};

// Wrap same counters into different objects to make sure that different key
// returns different objects as well as aggregate the usages.
struct CountersWrapper {
    CountersWrapper(Counters* c, bthread_key_t key) : _c(c), _key(key) {}
    ~CountersWrapper() {
        if (_c) {
            _c->ndestroy.fetch_add(1, butil::memory_order_relaxed);
        }
        CHECK_EQ(0, bthread_key_delete(_key));
    }
private:
    Counters* _c;
    bthread_key_t _key;
};

static void destroy_counters_wrapper(void* arg) {
    delete static_cast<CountersWrapper*>(arg);
}

const size_t NKEY_PER_WORKER = 32;

// NOTE: returns void to use ASSERT
static void worker1_impl(Counters* cs) {
    cs->nenterthread.fetch_add(1, butil::memory_order_relaxed);
    bthread_key_t k[NKEY_PER_WORKER];
    CountersWrapper* ws[arraysize(k)];
    for (size_t i = 0; i < arraysize(k); ++i) {
        ASSERT_EQ(0, bthread_key_create(&k[i], destroy_counters_wrapper));
    }
    for (size_t i = 0; i < arraysize(k); ++i) {
        ws[i] = new CountersWrapper(cs, k[i]);
    }
    // Get just-created tls should return NULL.
    for (size_t i = 0; i < arraysize(k); ++i) {
        ASSERT_EQ(NULL, bthread_getspecific(k[i]));
    }
    for (size_t i = 0; i < arraysize(k); ++i) {
        cs->ncreate.fetch_add(1, butil::memory_order_relaxed);
        ASSERT_EQ(0, bthread_setspecific(k[i], ws[i]))
            << "i=" << i << " is_bthread=" << !!bthread_self();
            
    }
    // Sleep a while to make some context switches. TLS should be unchanged.
    bthread_usleep(10000);
    
    for (size_t i = 0; i < arraysize(k); ++i) {
        ASSERT_EQ(ws[i], bthread_getspecific(k[i])) << "i=" << i;
    }
    cs->nleavethread.fetch_add(1, butil::memory_order_relaxed);
}

static void* worker1(void* arg) {
    worker1_impl(static_cast<Counters*>(arg));
    return NULL;
}

TEST(KeyTest, creating_key_in_parallel) {
    Counters args;
    pthread_t th[8];
    bthread_t bth[8];
    for (size_t i = 0; i < arraysize(th); ++i) {
        ASSERT_EQ(0, pthread_create(&th[i], NULL, worker1, &args));
    }
    for (size_t i = 0; i < arraysize(bth); ++i) {
        ASSERT_EQ(0, bthread_start_background(&bth[i], NULL, worker1, &args));
    }
    for (size_t i = 0; i < arraysize(th); ++i) {
        ASSERT_EQ(0, pthread_join(th[i], NULL));
    }
    for (size_t i = 0; i < arraysize(bth); ++i) {
        ASSERT_EQ(0, bthread_join(bth[i], NULL));
    }
    ASSERT_EQ(arraysize(th) + arraysize(bth),
              args.nenterthread.load(butil::memory_order_relaxed));
    ASSERT_EQ(arraysize(th) + arraysize(bth),
              args.nleavethread.load(butil::memory_order_relaxed));
    ASSERT_EQ(NKEY_PER_WORKER * (arraysize(th) + arraysize(bth)),
              args.ncreate.load(butil::memory_order_relaxed));
    ASSERT_EQ(NKEY_PER_WORKER * (arraysize(th) + arraysize(bth)),
              args.ndestroy.load(butil::memory_order_relaxed));
}

butil::atomic<size_t> seq(1);
std::vector<size_t> seqs;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void dtor2(void* arg) {
    BAIDU_SCOPED_LOCK(mutex);
    seqs.push_back((size_t)arg);
}

// NOTE: returns void to use ASSERT
static void worker2_impl(bthread_key_t k) {
    ASSERT_EQ(NULL, bthread_getspecific(k));
    ASSERT_EQ(0, bthread_setspecific(k, (void*)seq.fetch_add(1)));
}

static void* worker2(void* arg) {
    worker2_impl(*static_cast<bthread_key_t*>(arg));
    return NULL;
}

TEST(KeyTest, use_one_key_in_different_threads) {
    bthread_key_t k;
    ASSERT_EQ(0, bthread_key_create(&k, dtor2)) << berror();
    seqs.clear();

    pthread_t th[16];
    for (size_t i = 0; i < arraysize(th); ++i) {
        ASSERT_EQ(0, pthread_create(&th[i], NULL, worker2, &k));
    }
    bthread_t bth[1];
    for (size_t i = 0; i < arraysize(bth); ++i) {
        ASSERT_EQ(0, bthread_start_urgent(&bth[i], NULL, worker2, &k));
    }
    for (size_t i = 0; i < arraysize(th); ++i) {
        ASSERT_EQ(0, pthread_join(th[i], NULL));
    }
    for (size_t i = 0; i < arraysize(bth); ++i) {
        ASSERT_EQ(0, bthread_join(bth[i], NULL));
    }
    ASSERT_EQ(arraysize(th) + arraysize(bth), seqs.size());
    std::sort(seqs.begin(), seqs.end());
    ASSERT_EQ(seqs.end(), std::unique(seqs.begin(), seqs.end()));
    ASSERT_EQ(arraysize(th) + arraysize(bth) - 1, *(seqs.end()-1) - *seqs.begin());

    ASSERT_EQ(0, bthread_key_delete(k));
}

struct Keys {
    bthread_key_t valid_key;
    bthread_key_t invalid_key;
};

void* const DUMMY_PTR = (void*)1;

void use_invalid_keys_impl(const Keys* keys) {
    ASSERT_EQ(NULL, bthread_getspecific(keys->invalid_key));
    // valid key returns NULL as well.
    ASSERT_EQ(NULL, bthread_getspecific(keys->valid_key));

    // both pthread_setspecific(of nptl) and bthread_setspecific should find
    // the key is invalid.
    ASSERT_EQ(EINVAL, bthread_setspecific(keys->invalid_key, DUMMY_PTR));
    ASSERT_EQ(0, bthread_setspecific(keys->valid_key, DUMMY_PTR));

    // Print error again.
    ASSERT_EQ(NULL, bthread_getspecific(keys->invalid_key));
    ASSERT_EQ(DUMMY_PTR, bthread_getspecific(keys->valid_key));
}

void* use_invalid_keys(void* args) {
    use_invalid_keys_impl(static_cast<const Keys*>(args));
    return NULL;
}

TEST(KeyTest, use_invalid_keys) {
    Keys keys;
    ASSERT_EQ(0, bthread_key_create(&keys.valid_key, NULL));
    // intended to be a created but invalid key.
    keys.invalid_key.index = keys.valid_key.index;
    keys.invalid_key.version = 123;

    pthread_t th;
    bthread_t bth;
    ASSERT_EQ(0, pthread_create(&th, NULL, use_invalid_keys, &keys));
    ASSERT_EQ(0, bthread_start_urgent(&bth, NULL, use_invalid_keys, &keys));
    ASSERT_EQ(0, pthread_join(th, NULL));
    ASSERT_EQ(0, bthread_join(bth, NULL));
    ASSERT_EQ(0, bthread_key_delete(keys.valid_key));
}

TEST(KeyTest, reuse_key) {
    bthread_key_t key;
    ASSERT_EQ(0, bthread_key_create(&key, NULL));
    ASSERT_EQ(NULL, bthread_getspecific(key));
    ASSERT_EQ(0, bthread_setspecific(key, (void*)1));
    ASSERT_EQ(0, bthread_key_delete(key)); // delete key before clearing TLS.
    bthread_key_t key2;
    ASSERT_EQ(0, bthread_key_create(&key2, NULL));
    ASSERT_EQ(key.index, key2.index);
    // The slot is not NULL, the impl must check version and return NULL.
    ASSERT_EQ(NULL, bthread_getspecific(key2));
}

// NOTE: sid is short for 'set in dtor'.
struct SidData {
    bthread_key_t key;
    int seq;
    int end_seq;
};

static void sid_dtor(void* tls){
    SidData* data = (SidData*)tls;
    // Should already be set NULL.
    ASSERT_EQ(NULL, bthread_getspecific(data->key));
    if (++data->seq < data->end_seq){
        ASSERT_EQ(0, bthread_setspecific(data->key, data));
    }
}

static void sid_thread_impl(SidData* data) {
    ASSERT_EQ(0, bthread_setspecific(data->key, data));
};

static void* sid_thread(void* args) {
    sid_thread_impl((SidData*)args);
    return NULL;
}

TEST(KeyTest, set_in_dtor) {
    bthread_key_t key;
    ASSERT_EQ(0, bthread_key_create(&key, sid_dtor));

    SidData pth_data = { key, 0, 3 };
    SidData bth_data = { key, 0, 3 };
    SidData bth2_data = { key, 0, 3 };
    
    pthread_t pth;
    bthread_t bth;
    bthread_t bth2;
    ASSERT_EQ(0, pthread_create(&pth, NULL, sid_thread, &pth_data));
    ASSERT_EQ(0, bthread_start_urgent(&bth, NULL, sid_thread, &bth_data));
    ASSERT_EQ(0, bthread_start_urgent(&bth2, &BTHREAD_ATTR_PTHREAD,
                                      sid_thread, &bth2_data));

    ASSERT_EQ(0, pthread_join(pth, NULL));
    ASSERT_EQ(0, bthread_join(bth, NULL));
    ASSERT_EQ(0, bthread_join(bth2, NULL));
        
    ASSERT_EQ(0, bthread_key_delete(key));

    EXPECT_EQ(pth_data.end_seq, pth_data.seq);
    EXPECT_EQ(bth_data.end_seq, bth_data.seq);
    EXPECT_EQ(bth2_data.end_seq, bth2_data.seq);
}

struct SBAData {
    bthread_key_t key;
    int level;
    int ndestroy;
};

struct SBATLS {
    int* ndestroy;
    
    static void deleter(void* d) {
        SBATLS* tls = (SBATLS*)d;
        ++*tls->ndestroy;
        delete tls;
    }
};

void* set_before_anybth(void* args);

void set_before_anybth_impl(SBAData* data) {
    ASSERT_EQ(NULL, bthread_getspecific(data->key));
    SBATLS *tls = new SBATLS;
    tls->ndestroy = &data->ndestroy;
    ASSERT_EQ(0, bthread_setspecific(data->key, tls));
    ASSERT_EQ(tls, bthread_getspecific(data->key));
    if (data->level++ == 0) {
        bthread_t bth;
        ASSERT_EQ(0, bthread_start_urgent(&bth, NULL, set_before_anybth, data));
        ASSERT_EQ(0, bthread_join(bth, NULL));
        ASSERT_EQ(1, data->ndestroy);
    } else {
        bthread_usleep(1000);
    }
    ASSERT_EQ(tls, bthread_getspecific(data->key));
}

void* set_before_anybth(void* args) {
    set_before_anybth_impl((SBAData*)args);
    return NULL;
}

TEST(KeyTest, set_tls_before_creating_any_bthread) {
    bthread_key_t key;
    ASSERT_EQ(0, bthread_key_create(&key, SBATLS::deleter));
    pthread_t th;
    SBAData data;
    data.key = key;
    data.level = 0;
    data.ndestroy = 0;
    ASSERT_EQ(0, pthread_create(&th, NULL, set_before_anybth, &data));
    ASSERT_EQ(0, pthread_join(th, NULL));
    ASSERT_EQ(0, bthread_key_delete(key));
    ASSERT_EQ(2, data.level);
    ASSERT_EQ(2, data.ndestroy);
}

struct PoolData {
    bthread_key_t key;
    PoolData* data;
    int seq;
    int end_seq;
};

bool use_same_keytable = false;

static void pool_thread_impl(PoolData* data) {
    if (NULL == bthread_getspecific(data->key)) {
        ASSERT_EQ(0, bthread_setspecific(data->key, data));
    } else {
        use_same_keytable = true;
    }
};

static void* pool_thread(void* args) {
    pool_thread_impl((PoolData*)args);
    return NULL;
}

static void pool_dtor(void* tls){
    PoolData* data = (PoolData*)tls;
    // Should already be set NULL.
    ASSERT_EQ(NULL, bthread_getspecific(data->key));
    if (++data->seq < data->end_seq){
        ASSERT_EQ(0, bthread_setspecific(data->key, data));
    }
}

TEST(KeyTest, using_pool) {
    bthread_key_t key;
    ASSERT_EQ(0, bthread_key_create(&key, pool_dtor));

    bthread_keytable_pool_t pool;
    ASSERT_EQ(0, bthread_keytable_pool_init(&pool));
    ASSERT_EQ(0, bthread_keytable_pool_size(&pool));

    bthread_attr_t attr;
    ASSERT_EQ(0, bthread_attr_init(&attr));
    attr.keytable_pool = &pool;

    bthread_attr_t attr2 = attr;
    attr2.stack_type = BTHREAD_STACKTYPE_PTHREAD;

    PoolData bth_data = { key, NULL, 0, 3 };
    bthread_t bth;
    ASSERT_EQ(0, bthread_start_urgent(&bth, &attr, pool_thread, &bth_data));
    ASSERT_EQ(0, bthread_join(bth, NULL));
    ASSERT_EQ(0, bth_data.seq);

    PoolData bth2_data = { key, NULL, 0, 3 };
    bthread_t bth2;
    ASSERT_EQ(0, bthread_start_urgent(&bth2, &attr2, pool_thread, &bth2_data));
    ASSERT_EQ(0, bthread_join(bth2, NULL));
    ASSERT_EQ(0, bth2_data.seq);
        
    ASSERT_EQ(0, bthread_keytable_pool_destroy(&pool));
    if (use_same_keytable) {
        EXPECT_EQ(bth_data.end_seq, bth_data.seq);
        EXPECT_EQ(0, bth2_data.seq); 
    } else {
        EXPECT_EQ(bth_data.end_seq, bth_data.seq);
        EXPECT_EQ(bth_data.end_seq, bth2_data.seq);
    }

    ASSERT_EQ(0, bthread_key_delete(key));
}

bthread_keytable_pool_t test_pool;
struct PoolData2 {
    bthread_key_t key;
    bthread_attr_t attr;
};

static void pool_dtor2(void* tls) {
    delete static_cast<PoolData2*>(tls);
}

static void usleep_thread_impl(PoolData2* data) {
    if (NULL == bthread_getspecific(data->key)) {
        PoolData2* data_new = new PoolData2();
        ASSERT_EQ(0, bthread_setspecific(data->key, data_new));
    }
    bthread_usleep(1000L);
    int length = get_thread_local_keytable_list_length(&test_pool);
    ASSERT_LE((size_t)length, bthread::FLAGS_key_table_list_size);
}

static void* usleep_thread(void* args) {
    usleep_thread_impl((PoolData2*)args);
    return NULL;
}

static void launch_many_bthreads(PoolData2* data) {
    std::vector<bthread_t> tids;
    tids.reserve(25000);
    for (size_t i = 0; i < 25000; ++i) {
        bthread_t t0;
        PoolData2* data_tmp = new PoolData2();
        data_tmp->key = data->key;
        ASSERT_EQ(0, bthread_start_background(&t0, &(data->attr), usleep_thread, data_tmp));
        tids.push_back(t0);
    }

    usleep(3 * 1000 * 1000L);
    for (size_t i = 0; i < tids.size(); ++i) {
        bthread_join(tids[i], NULL);
    }
}

static void* run_launch_many_bthreads(void* args) {
    PoolData2* data = (PoolData2*)args;
    launch_many_bthreads(data);
    return NULL;
}

TEST(KeyTest, frequently_borrow_keytable_when_using_pool) {
    PoolData2 data;
    ASSERT_EQ(0, bthread_key_create(&data.key, pool_dtor2));

    ASSERT_EQ(0, bthread_keytable_pool_init(&test_pool));
    ASSERT_EQ(0, bthread_keytable_pool_size(&test_pool));

    ASSERT_EQ(0, bthread_attr_init(&data.attr));
    data.attr.keytable_pool = &test_pool;

    bthread_t bth;
    ASSERT_EQ(0, bthread_start_urgent(&bth, &data.attr, run_launch_many_bthreads, &data));
    ASSERT_EQ(0, bthread_join(bth, NULL));
    std::cout << "Free keytable size is "
              << bthread_keytable_pool_size(&test_pool)
              << " use keytable size is 25000" << std::endl;
    ASSERT_EQ(0, bthread_keytable_pool_destroy(&test_pool));
    ASSERT_EQ(0, bthread_key_delete(data.key));
}

std::vector<bthread::KeyTable*> table_list;
pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

static void return_thread_impl() {
    for (int i = 0; i < 32768; i++) {
        {
            BAIDU_SCOPED_LOCK(table_mutex);
            if (table_list.size() > 0) {
                bthread::KeyTable* keytable = table_list[0];
                table_list.erase(table_list.begin());
                bthread::return_keytable(&test_pool, keytable);
            }
        }
        int length = get_thread_local_keytable_list_length(&test_pool);
        ASSERT_LE((size_t)length, bthread::FLAGS_key_table_list_size);
    }
}

static void* return_thread(void*) {
    return_thread_impl();
    return NULL;
}

static void borrow_thread_impl() {
    for (int i = 0; i < 32768; i++) {
        bthread::KeyTable* keytable = bthread::borrow_keytable(&test_pool);
        BAIDU_SCOPED_LOCK(table_mutex);
        table_list.push_back(keytable);
    }
}

static void* borrow_thread(void*) {
    borrow_thread_impl();
    return NULL;
}

TEST(KeyTest, borrow_and_return_keytable_when_using_pool) {
    ASSERT_EQ(0, bthread_keytable_pool_init(&test_pool));
    ASSERT_EQ(0, bthread_keytable_pool_size(&test_pool));

    bthread_attr_t attr;
    ASSERT_EQ(0, bthread_attr_init(&attr));
    attr.keytable_pool = &test_pool;

    bthread_t borrow_bth[8];
    bthread_t return_bth[8];
    for (size_t i = 0; i < arraysize(borrow_bth); ++i) {
        ASSERT_EQ(0, bthread_start_background(&borrow_bth[i], &attr,
                                              borrow_thread, NULL));
    }
    for (size_t i = 0; i < arraysize(return_bth); ++i) {
        ASSERT_EQ(0, bthread_start_background(&return_bth[i], &attr,
                                              return_thread, NULL));
    }

    for (size_t i = 0; i < arraysize(borrow_bth); ++i) {
        ASSERT_EQ(0, bthread_join(borrow_bth[i], NULL));
    }
    for (size_t i = 0; i < arraysize(return_bth); ++i) {
        ASSERT_EQ(0, bthread_join(return_bth[i], NULL));
    }

    for (size_t i = 0; i < table_list.size(); i++) {
        bthread::return_keytable(&test_pool, table_list[i]);
    }
    table_list.clear();

    ASSERT_EQ(0, bthread_keytable_pool_destroy(&test_pool));
}

// NOTE: lid is short for 'lock in dtor'.
butil::atomic<size_t> lid_seq(1);
std::vector<size_t> lid_seqs;
bthread_mutex_t mu;

static void lid_dtor(void* tls) {
    bthread_mutex_lock(&mu);
    lid_seqs.push_back((size_t)tls);
    bthread_mutex_unlock(&mu);
}

static void lid_worker_impl(bthread_key_t key) {
    ASSERT_EQ(NULL, bthread_getspecific(key));
    ASSERT_EQ(0, bthread_setspecific(key, (void*)lid_seq.fetch_add(1)));
}

static void* lid_worker(void* arg) {
    lid_worker_impl(*static_cast<bthread_key_t*>(arg));
    return NULL;
}

TEST(KeyTest, use_bthread_mutex_in_dtor) {
    bthread_key_t key;

    ASSERT_EQ(0, bthread_mutex_init(&mu, nullptr));
    ASSERT_EQ(0, bthread_key_create(&key, lid_dtor));

    lid_seqs.clear();

    bthread_t bth[8];
    for (size_t i = 0; i < arraysize(bth); ++i) {
        ASSERT_EQ(0, bthread_start_urgent(&bth[i], NULL, lid_worker, &key));
    }
    pthread_t th[8];
    for (size_t i = 0; i < arraysize(th); ++i) {
        ASSERT_EQ(0, pthread_create(&th[i], NULL, lid_worker, &key));
    }
    for (size_t i = 0; i < arraysize(bth); ++i) {
        ASSERT_EQ(0, bthread_join(bth[i], NULL));
    }
    for (size_t i = 0; i < arraysize(th); ++i) {
        ASSERT_EQ(0, pthread_join(th[i], NULL));
    }
    ASSERT_EQ(arraysize(th) + arraysize(bth), lid_seqs.size());
    std::sort(lid_seqs.begin(), lid_seqs.end());
    ASSERT_EQ(lid_seqs.end(), std::unique(lid_seqs.begin(), lid_seqs.end()));
    ASSERT_EQ(arraysize(th) + arraysize(bth) - 1,
              *(lid_seqs.end() - 1) - *lid_seqs.begin());

    ASSERT_EQ(0, bthread_key_delete(key));
    ASSERT_EQ(0, bthread_mutex_destroy(&mu));
}

}  // namespace
