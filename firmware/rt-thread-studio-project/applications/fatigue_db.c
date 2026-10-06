#include "fatigue_db.h"
#include "fatigue_db_builtin_profiles.h"
#include "emg_pipeline.h"

#include <rtdevice.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef FINSH_USING_MSH
#include <finsh.h>
#endif

#define DBG_TAG "fatdb"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define FATIGUE_DB_RUNTIME_ROOT  "/smart_knee_runtime_db"
#define FATIGUE_DB_ROOT          FATIGUE_DB_RUNTIME_ROOT "/DB_compatible"
#define FATIGUE_DB_USERS         FATIGUE_DB_ROOT "/users.csv"
#define FATIGUE_DB_SESSIONS      FATIGUE_DB_ROOT "/sessions.csv"
#define FATIGUE_DB_LIVE          FATIGUE_DB_ROOT "/live.csv"
#define FATIGUE_DB_REPS          FATIGUE_DB_ROOT "/reps.csv"
#define FATIGUE_DB_REFS          FATIGUE_DB_ROOT "/reference_features.csv"
#define FATIGUE_DB_MATCHES       FATIGUE_DB_ROOT "/matches.csv"
#define FATIGUE_DB_RUNTIME_UNITS FATIGUE_DB_RUNTIME_ROOT "/runtime_units_normalized.csv"
#define FATIGUE_DB_THREAD_STACK  4096
#define FATIGUE_DB_THREAD_PRIO   26
#define FATIGUE_DB_RETRY_MS      2000
#define FATIGUE_DB_LIVE_DIV      1
#define FATIGUE_DB_REF_LINE_MAX  320
#define FATIGUE_DB_RUNTIME_LINE_MAX 640
#define FATIGUE_DB_QUERY_ROWS    100
#define FATIGUE_DB_QUERY_MIN_ROWS 20
#define FATIGUE_DB_QUERY_MIN_ACTIVE_ROWS 20
#define FATIGUE_DB_TOP_K         5
#define FATIGUE_DB_MATCH_REFRESH_MS 15000
#define FATIGUE_DB_MAX_BLEND_PERCENT 25

typedef struct
{
    uint32_t rms;
    uint32_t mav;
    uint32_t wl;
    uint16_t zc;
    uint16_t ssc;
    int32_t angle_x10;
    uint8_t active;
} fatigue_db_query_sample_t;

typedef struct
{
    char source_family[20];
    char person_id[24];
    char unit_id[48];
    char label[20];
    char fatigue_level[12];
    float z[8];
} fatigue_db_runtime_row_t;

typedef struct
{
    rt_bool_t valid;
    fatigue_db_runtime_row_t row;
    float distance;
} fatigue_db_candidate_t;

typedef struct
{
    rt_mutex_t lock;
    rt_thread_t thread;
    rt_bool_t ready;
    rt_bool_t session_open;
    uint32_t live_count;
    uint32_t session_id;
    char user_id[24];
    char sex[8];
    char habit[16];
    char injury[16];
    char action_hint[16];
    uint16_t age;
    uint16_t height_cm;
    uint16_t weight_kg;
    uint16_t leg_cm;
    fatigue_db_match_t match;
    fatigue_db_query_sample_t query[FATIGUE_DB_QUERY_ROWS];
    uint16_t query_head;
    uint16_t query_count;
    uint64_t query_rms_sum;
    uint64_t query_mav_sum;
    uint64_t query_wl_sum;
    uint64_t query_zc_sum;
    uint64_t query_ssc_sum;
    uint32_t query_active_count;
    fatigue_db_runtime_match_t runtime_match;
    rt_tick_t runtime_last_match_tick;
} fatigue_db_state_t;

typedef struct
{
    char person_id[24];
    char sex[8];
    char habit[16];
    char injury[16];
    char action[16];
    uint16_t age;
    uint16_t height_cm;
    uint16_t weight_kg;
    uint16_t leg_cm;
    uint32_t base_rms;
    uint32_t base_mav;
    uint32_t base_wl;
    uint16_t base_zc;
    uint16_t base_ssc;
    uint16_t fatigue_light;
    uint16_t fatigue_mid;
    uint16_t fatigue_heavy;
} fatigue_db_ref_t;

static fatigue_db_state_t db;
static fatigue_db_runtime_match_t published_runtime_match;

/* Publish a complete match snapshot without making UI or wireless readers
 * wait for the database worker to finish a full file scan. */
static void db_publish_runtime_match(const fatigue_db_runtime_match_t *match)
{
    if (match == RT_NULL) return;

    rt_enter_critical();
    published_runtime_match = *match;
    rt_exit_critical();
}

#if defined(RT_USING_DFS) && defined(RT_USING_DFS_ELMFAT)

#include <dfs_file.h>
#include <dfs_fs.h>
#include <fcntl.h>

static struct dfs_fd users_fd;
static struct dfs_fd sessions_fd;
static struct dfs_fd live_fd;
static struct dfs_fd reps_fd;
static struct dfs_fd matches_fd;
static rt_bool_t users_open;
static rt_bool_t sessions_open;
static rt_bool_t live_open;
static rt_bool_t reps_open;
static rt_bool_t matches_open;

static const char *ref_header =
    "person_id,sex,age,height_cm,weight_kg,leg_cm,habit,injury,action,"
    "base_rms,base_mav,base_wl,base_zc,base_ssc,"
    "fatigue_light,fatigue_mid,fatigue_heavy\r\n";

static int db_write(struct dfs_fd *fd, const char *text)
{
    int len;

    if (fd == RT_NULL || text == RT_NULL) return -RT_EINVAL;

    len = (int)rt_strlen(text);
    if (len <= 0) return RT_EOK;

    return dfs_file_write(fd, text, (size_t)len) == len ? RT_EOK : -RT_ERROR;
}

static int db_mkdir(const char *path)
{
    struct dfs_fd fd;
    int result;

    rt_memset(&fd, 0, sizeof(fd));
    result = dfs_file_open(&fd, path, O_RDONLY | O_DIRECTORY);
    if (result == RT_EOK)
    {
        dfs_file_close(&fd);
        return RT_EOK;
    }

    rt_memset(&fd, 0, sizeof(fd));
    result = dfs_file_open(&fd, path, O_DIRECTORY | O_CREAT);
    if (result == RT_EOK)
    {
        dfs_file_close(&fd);
        return RT_EOK;
    }

    return result;
}

static int db_try_mount_root(void)
{
    struct statfs s;
    const char *devices[] =
    {
        "sd0p0", "sd0p1", "sd0",
        "udisk0", "udisk", "usb0",
        RT_NULL
    };

    if (dfs_statfs("/", &s) == RT_EOK)
    {
        return RT_EOK;
    }

    for (int i = 0; devices[i] != RT_NULL; i++)
    {
        if (rt_device_find(devices[i]) != RT_NULL &&
            dfs_mount(devices[i], "/", "elm", 0, 0) == RT_EOK)
        {
            rt_kprintf("DB,MOUNT,%s,/\n", devices[i]);
            return RT_EOK;
        }
    }

    return -RT_ERROR;
}

#ifndef RT_USING_DFS_MNTTABLE
int dfs_mount_device(rt_device_t dev)
{
    if (dev == RT_NULL) return -RT_ERROR;

    if (dfs_filesystem_get_mounted_path(dev) != RT_NULL)
    {
        return RT_EOK;
    }

    if (dfs_mount(dev->parent.name, "/", "elm", 0, 0) == RT_EOK)
    {
        rt_kprintf("DB,AUTO_MOUNT,%s,/\n", dev->parent.name);
        return RT_EOK;
    }

    return -RT_ERROR;
}

int dfs_unmount_device(rt_device_t dev)
{
    const char *path;

    if (dev == RT_NULL) return -RT_ERROR;

    path = dfs_filesystem_get_mounted_path(dev);
    if (path == RT_NULL) return -RT_ERROR;

    return dfs_unmount(path) == RT_EOK ? RT_EOK : -RT_ERROR;
}
#endif

static int db_open_csv(struct dfs_fd *fd, rt_bool_t *opened,
                       const char *path, const char *header)
{
    int result;
    rt_bool_t empty;

    if (*opened) return RT_EOK;

    rt_memset(fd, 0, sizeof(*fd));
    result = dfs_file_open(fd, path, O_WRONLY | O_CREAT | O_APPEND);
    if (result != RT_EOK)
    {
        return result;
    }

    empty = (fd->size == 0) ? RT_TRUE : RT_FALSE;
    *opened = RT_TRUE;

    if (empty)
    {
        db_write(fd, header);
        dfs_file_flush(fd);
    }

    return RT_EOK;
}

static void db_close_all(void)
{
    if (users_open)    { dfs_file_close(&users_fd); users_open = RT_FALSE; }
    if (sessions_open) { dfs_file_close(&sessions_fd); sessions_open = RT_FALSE; }
    if (live_open)     { dfs_file_close(&live_fd); live_open = RT_FALSE; }
    if (reps_open)     { dfs_file_close(&reps_fd); reps_open = RT_FALSE; }
    if (matches_open)  { dfs_file_close(&matches_fd); matches_open = RT_FALSE; }
}

static int db_ensure_reference_file(void)
{
    struct dfs_fd fd;
    int result;

    rt_memset(&fd, 0, sizeof(fd));
    result = dfs_file_open(&fd, FATIGUE_DB_REFS, O_RDONLY);
    if (result == RT_EOK)
    {
        dfs_file_close(&fd);
        return RT_EOK;
    }

    rt_memset(&fd, 0, sizeof(fd));
    result = dfs_file_open(&fd, FATIGUE_DB_REFS, O_WRONLY | O_CREAT);
    if (result != RT_EOK)
    {
        return result;
    }

    db_write(&fd, ref_header);
    dfs_file_flush(&fd);
    dfs_file_close(&fd);
    return RT_EOK;
}

static int db_prepare_files(void)
{
    if (db_try_mount_root() != RT_EOK)
    {
        return -RT_ERROR;
    }

    if (db_mkdir(FATIGUE_DB_RUNTIME_ROOT) != RT_EOK)
    {
        return -RT_ERROR;
    }

    if (db_mkdir(FATIGUE_DB_ROOT) != RT_EOK)
    {
        return -RT_ERROR;
    }

    if (db_open_csv(&users_fd, &users_open, FATIGUE_DB_USERS,
                    "write_ms,user_id,sex,age,height_cm,weight_kg,leg_cm,habit,injury,note\r\n") != RT_EOK)
        return -RT_ERROR;

    if (db_open_csv(&sessions_fd, &sessions_open, FATIGUE_DB_SESSIONS,
                    "session_id,user_id,event_ms,event,action,rest_rms,rest_mav,base_rms,base_mav,base_wl,base_zc,base_ssc,note\r\n") != RT_EOK)
        return -RT_ERROR;

    if (db_open_csv(&live_fd, &live_open, FATIGUE_DB_LIVE,
                    "session_id,user_id,t_ms,ac,rms,mav,wl,zc,ssc,zcr_x1000,fatigue_score,alert,active,emg_phase,emg_status,motion_phase,angle_x10,ax,ay,az,gx,gy,gz\r\n") != RT_EOK)
        return -RT_ERROR;

    if (db_open_csv(&reps_fd, &reps_open, FATIGUE_DB_REPS,
                    "session_id,user_id,rep,action,depth_x10,t_dn_ms,t_bot_ms,t_up_ms,iemg,mean_mav,mean_rms,mean_wl,peak_rms,peak_fatigue,quality_score,quality_label,fail_mask,base_mav\r\n") != RT_EOK)
        return -RT_ERROR;

    if (db_open_csv(&matches_fd, &matches_open, FATIGUE_DB_MATCHES,
                    "session_id,user_id,event_ms,action,match_person,score,ref_base_rms,ref_base_mav,ref_base_wl,ref_base_zc,ref_base_ssc,fatigue_light,fatigue_mid,fatigue_heavy\r\n") != RT_EOK)
        return -RT_ERROR;

    if (db_ensure_reference_file() != RT_EOK)
        return -RT_ERROR;

    return RT_EOK;
}

static void db_write_session_event(const char *event, const char *note)
{
    char line[256];
    emg_snapshot_t s;
    uint32_t t_ms = rt_tick_get_millisecond();

    if (!sessions_open || event == RT_NULL) return;

    emg_pipeline_get_snapshot(&s);
    rt_snprintf(line, sizeof(line),
                "%lu,%s,%lu,%s,%s,%lu,%lu,%lu,%lu,%lu,%u,%u,%s\r\n",
                (unsigned long)db.session_id,
                db.user_id,
                (unsigned long)t_ms,
                event,
                db.action_hint,
                (unsigned long)s.rest_rms,
                (unsigned long)s.rest_mav,
                (unsigned long)s.base_rms,
                (unsigned long)s.base_mav,
                (unsigned long)s.base_wl,
                (unsigned)s.base_zc,
                (unsigned)s.base_ssc,
                note ? note : "");
    db_write(&sessions_fd, line);
    dfs_file_flush(&sessions_fd);
}

static int db_read_line(struct dfs_fd *fd, char *line, rt_size_t max_len)
{
    int n = 0;
    char c;

    if (fd == RT_NULL || line == RT_NULL || max_len < 2U) return -RT_EINVAL;

    while ((rt_size_t)n < max_len - 1U)
    {
        int r = dfs_file_read(fd, &c, 1);
        if (r <= 0) break;
        if (c == '\r') continue;
        line[n++] = c;
        if (c == '\n') break;
    }

    line[n] = '\0';
    return n;
}

static char *db_csv_next(char **cursor)
{
    char *start;
    char *p;

    if (cursor == RT_NULL || *cursor == RT_NULL) return RT_NULL;

    start = *cursor;
    p = start;
    while (*p != '\0' && *p != ',' && *p != '\n' && *p != '\r')
    {
        p++;
    }

    if (*p == ',')
    {
        *p = '\0';
        *cursor = p + 1;
    }
    else
    {
        *p = '\0';
        *cursor = p;
    }

    return start;
}

static uint32_t db_u32(const char *s)
{
    if (s == RT_NULL || *s == '\0') return 0;
    return (uint32_t)strtoul(s, RT_NULL, 10);
}

static float db_float(const char *s)
{
    float value = 0.0f;
    float scale = 0.1f;
    int sign = 1;
    int exponent = 0;
    int exponent_sign = 1;

    if (s == RT_NULL) return 0.0f;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    while (*s >= '0' && *s <= '9')
    {
        value = value * 10.0f + (float)(*s - '0');
        s++;
    }
    if (*s == '.')
    {
        s++;
        while (*s >= '0' && *s <= '9')
        {
            value += (float)(*s - '0') * scale;
            scale *= 0.1f;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E')
    {
        s++;
        if (*s == '-') { exponent_sign = -1; s++; }
        else if (*s == '+') { s++; }
        while (*s >= '0' && *s <= '9')
        {
            exponent = exponent * 10 + (*s - '0');
            s++;
        }
        while (exponent-- > 0)
            value *= exponent_sign > 0 ? 10.0f : 0.1f;
    }
    return sign < 0 ? -value : value;
}

static void db_copy(char *dst, rt_size_t dst_len, const char *src)
{
    if (dst == RT_NULL || dst_len == 0U) return;
    if (src == RT_NULL) src = "";
    rt_strncpy(dst, src, dst_len - 1U);
    dst[dst_len - 1U] = '\0';
}

static void db_query_reset(void)
{
    db.query_head = 0;
    db.query_count = 0;
    db.query_rms_sum = 0;
    db.query_mav_sum = 0;
    db.query_wl_sum = 0;
    db.query_zc_sum = 0;
    db.query_ssc_sum = 0;
    db.query_active_count = 0;
    db.runtime_last_match_tick = 0;
    rt_memset(db.query, 0, sizeof(db.query));
    rt_memset(&db.runtime_match, 0, sizeof(db.runtime_match));
    db_publish_runtime_match(&db.runtime_match);
}

static void db_query_push(const fatigue_db_live_record_t *rec)
{
    fatigue_db_query_sample_t *slot;

    if (rec == RT_NULL) return;

    slot = &db.query[db.query_head];
    if (db.query_count >= FATIGUE_DB_QUERY_ROWS)
    {
        db.query_rms_sum -= slot->rms;
        db.query_mav_sum -= slot->mav;
        db.query_wl_sum -= slot->wl;
        db.query_zc_sum -= slot->zc;
        db.query_ssc_sum -= slot->ssc;
        if (slot->active && db.query_active_count > 0U) db.query_active_count--;
    }
    else
    {
        db.query_count++;
    }

    slot->rms = rec->rms;
    slot->mav = rec->mav;
    slot->wl = rec->wl;
    slot->zc = rec->zc;
    slot->ssc = rec->ssc;
    slot->angle_x10 = rec->angle_x10;
    slot->active = rec->active ? 1U : 0U;

    db.query_rms_sum += slot->rms;
    db.query_mav_sum += slot->mav;
    db.query_wl_sum += slot->wl;
    db.query_zc_sum += slot->zc;
    db.query_ssc_sum += slot->ssc;
    if (slot->active) db.query_active_count++;

    db.query_head = (uint16_t)((db.query_head + 1U) % FATIGUE_DB_QUERY_ROWS);
}

static rt_bool_t db_parse_ref_line(char *line, fatigue_db_ref_t *ref)
{
    char *cursor;
    char *f;

    if (line == RT_NULL || ref == RT_NULL) return RT_FALSE;
    if (line[0] == '\0' || line[0] == '\n') return RT_FALSE;
    if (rt_strncmp(line, "person_id", 9) == 0) return RT_FALSE;

    rt_memset(ref, 0, sizeof(*ref));
    cursor = line;

    f = db_csv_next(&cursor); db_copy(ref->person_id, sizeof(ref->person_id), f);
    f = db_csv_next(&cursor); db_copy(ref->sex, sizeof(ref->sex), f);
    f = db_csv_next(&cursor); ref->age = (uint16_t)db_u32(f);
    f = db_csv_next(&cursor); ref->height_cm = (uint16_t)db_u32(f);
    f = db_csv_next(&cursor); ref->weight_kg = (uint16_t)db_u32(f);
    f = db_csv_next(&cursor); ref->leg_cm = (uint16_t)db_u32(f);
    f = db_csv_next(&cursor); db_copy(ref->habit, sizeof(ref->habit), f);
    f = db_csv_next(&cursor); db_copy(ref->injury, sizeof(ref->injury), f);
    f = db_csv_next(&cursor); db_copy(ref->action, sizeof(ref->action), f);
    f = db_csv_next(&cursor); ref->base_rms = db_u32(f);
    f = db_csv_next(&cursor); ref->base_mav = db_u32(f);
    f = db_csv_next(&cursor); ref->base_wl = db_u32(f);
    f = db_csv_next(&cursor); ref->base_zc = (uint16_t)db_u32(f);
    f = db_csv_next(&cursor); ref->base_ssc = (uint16_t)db_u32(f);
    f = db_csv_next(&cursor); ref->fatigue_light = (uint16_t)db_u32(f);
    f = db_csv_next(&cursor); ref->fatigue_mid = (uint16_t)db_u32(f);
    f = db_csv_next(&cursor); ref->fatigue_heavy = (uint16_t)db_u32(f);

    return ref->person_id[0] != '\0' ? RT_TRUE : RT_FALSE;
}

static uint32_t db_abs_diff_u32(uint32_t a, uint32_t b)
{
    return a > b ? (a - b) : (b - a);
}

static uint32_t db_pct_diff(uint32_t a, uint32_t b)
{
    uint32_t den = a > b ? a : b;
    if (den == 0U) return 0U;
    return (uint32_t)(((uint64_t)db_abs_diff_u32(a, b) * 100ULL) / den);
}

static uint32_t db_text_penalty(const char *a, const char *b, uint32_t penalty)
{
    if (a == RT_NULL || b == RT_NULL || a[0] == '\0' || b[0] == '\0') return 0U;
    return rt_strcmp(a, b) == 0 ? 0U : penalty;
}

static uint32_t db_ref_score(const fatigue_db_ref_t *ref,
                             const emg_snapshot_t *snap,
                             const char *action)
{
    uint32_t score = 0;

    if (ref == RT_NULL || snap == RT_NULL) return 0xFFFFFFFFU;

    if (action != RT_NULL && action[0] != '\0' &&
        rt_strcmp(action, "mixed") != 0 &&
        ref->action[0] != '\0' &&
        rt_strcmp(ref->action, action) != 0)
    {
        score += 100000U;
    }

    score += db_text_penalty(db.sex, ref->sex, 200U);
    score += db_text_penalty(db.habit, ref->habit, 60U);
    score += db_text_penalty(db.injury, ref->injury, 100U);

    if (db.age > 0U && ref->age > 0U)
        score += db_abs_diff_u32(db.age, ref->age) * 3U;
    if (db.height_cm > 0U && ref->height_cm > 0U)
        score += db_abs_diff_u32(db.height_cm, ref->height_cm) * 2U;
    if (db.weight_kg > 0U && ref->weight_kg > 0U)
        score += db_abs_diff_u32(db.weight_kg, ref->weight_kg) * 3U;
    if (db.leg_cm > 0U && ref->leg_cm > 0U)
        score += db_abs_diff_u32(db.leg_cm, ref->leg_cm) * 6U;

    if (snap->base_rms > 0U || snap->base_mav > 0U)
    {
        score += db_pct_diff(snap->base_rms, ref->base_rms) * 5U;
        score += db_pct_diff(snap->base_mav, ref->base_mav) * 6U;
        score += db_pct_diff(snap->base_wl,  ref->base_wl)  * 2U;
        score += db_pct_diff(snap->base_zc,  ref->base_zc)  * 4U;
        score += db_pct_diff(snap->base_ssc, ref->base_ssc) * 4U;
    }

    return score;
}

static void db_match_from_ref(fatigue_db_match_t *match,
                              const fatigue_db_ref_t *ref,
                              uint32_t score,
                              const char *action)
{
    rt_memset(match, 0, sizeof(*match));
    match->matched = RT_TRUE;
    db_copy(match->person_id, sizeof(match->person_id), ref->person_id);
    db_copy(match->action, sizeof(match->action),
            action && action[0] ? action : ref->action);
    match->score = score;
    match->base_rms = ref->base_rms;
    match->base_mav = ref->base_mav;
    match->base_wl = ref->base_wl;
    match->base_zc = ref->base_zc;
    match->base_ssc = ref->base_ssc;
    match->fatigue_light = ref->fatigue_light;
    match->fatigue_mid = ref->fatigue_mid;
    match->fatigue_heavy = ref->fatigue_heavy;
}

static void db_write_match_event(const fatigue_db_match_t *match)
{
    char line[256];

    if (!matches_open || match == RT_NULL || !match->matched) return;

    rt_snprintf(line, sizeof(line),
                "%lu,%s,%lu,%s,%s,%lu,%lu,%lu,%lu,%u,%u,%u,%u,%u\r\n",
                (unsigned long)db.session_id,
                db.user_id,
                (unsigned long)rt_tick_get_millisecond(),
                match->action,
                match->person_id,
                (unsigned long)match->score,
                (unsigned long)match->base_rms,
                (unsigned long)match->base_mav,
                (unsigned long)match->base_wl,
                (unsigned)match->base_zc,
                (unsigned)match->base_ssc,
                (unsigned)match->fatigue_light,
                (unsigned)match->fatigue_mid,
                (unsigned)match->fatigue_heavy);
    db_write(&matches_fd, line);
    dfs_file_flush(&matches_fd);
}

rt_err_t fatigue_db_match_current(const char *action_hint, fatigue_db_match_t *out)
{
    struct dfs_fd fd;
    char line[FATIGUE_DB_REF_LINE_MAX];
    emg_snapshot_t snap;
    fatigue_db_match_t best_match;
    uint32_t best_score = 0xFFFFFFFFU;
    uint32_t row_count = 0;
    const char *action;

    if (db.lock == RT_NULL) return -RT_ERROR;

    rt_mutex_take(db.lock, RT_WAITING_FOREVER);
    if (!db.ready)
    {
        rt_mutex_release(db.lock);
        return -RT_ERROR;
    }

    action = (action_hint != RT_NULL && action_hint[0] != '\0') ?
             action_hint : db.action_hint;

    rt_memset(&fd, 0, sizeof(fd));
    if (dfs_file_open(&fd, FATIGUE_DB_REFS, O_RDONLY) != RT_EOK)
    {
        db_ensure_reference_file();
        rt_mutex_release(db.lock);
        return -RT_ENOSYS;
    }

    emg_pipeline_get_snapshot(&snap);
    rt_memset(&best_match, 0, sizeof(best_match));

    while (db_read_line(&fd, line, sizeof(line)) > 0)
    {
        fatigue_db_ref_t ref;
        uint32_t score;

        if (!db_parse_ref_line(line, &ref)) continue;
        row_count++;
        score = db_ref_score(&ref, &snap, action);
        if (score < best_score)
        {
            best_score = score;
            db_match_from_ref(&best_match, &ref, score, action);
        }
    }

    dfs_file_close(&fd);

    if (!best_match.matched)
    {
        rt_memset(&db.match, 0, sizeof(db.match));
        rt_mutex_release(db.lock);
        rt_kprintf("DB,MATCH,NONE,rows=%lu,file=%s\n",
                   (unsigned long)row_count, FATIGUE_DB_REFS);
        return -RT_ERROR;
    }

    db.match = best_match;
    db_write_match_event(&db.match);
    if (out != RT_NULL) *out = db.match;

    rt_kprintf("DB,MATCH,%s,score=%lu,action=%s,ref_rms=%lu,ref_mav=%lu,light=%u,mid=%u,heavy=%u\n",
               db.match.person_id,
               (unsigned long)db.match.score,
               db.match.action,
               (unsigned long)db.match.base_rms,
               (unsigned long)db.match.base_mav,
               (unsigned)db.match.fatigue_light,
               (unsigned)db.match.fatigue_mid,
               (unsigned)db.match.fatigue_heavy);

    rt_mutex_release(db.lock);
    return RT_EOK;
}

static const float runtime_mean[8] =
{
    4.275908932f, 4.084594807f, 4.391509216f, 1.607831710f,
    0.033329175f, 0.236828631f, 23.698390805f, 0.675465517f
};

static const float runtime_std[8] =
{
    0.771873309f, 0.746879912f, 0.769276857f, 0.667946269f,
    0.044015146f, 0.227845458f, 17.561831580f, 0.425446400f
};

static const float runtime_weight[8] =
{
    1.4f, 1.2f, 1.0f, 0.8f, 0.5f, 0.5f, 0.6f, 0.3f
};

static float db_log10_safe(float value)
{
    if (value < 0.0f) value = -value;
    if (value < 1.0e-12f) value = 1.0e-12f;
    return log10f(value);
}

static rt_bool_t db_runtime_build_query(float z[8])
{
    float feature[8];
    float n;
    float rms;
    float mav;
    float wl;
    float zc;
    float ssc;
    int32_t min_angle;
    int32_t max_angle;
    uint16_t i;

    if (z == RT_NULL || db.query_count < FATIGUE_DB_QUERY_MIN_ROWS)
        return RT_FALSE;

    n = (float)db.query_count;
    rms = (float)((double)db.query_rms_sum / (double)db.query_count);
    mav = (float)((double)db.query_mav_sum / (double)db.query_count);
    wl = (float)((double)db.query_wl_sum / (double)db.query_count);
    zc = (float)((double)db.query_zc_sum / (double)db.query_count);
    ssc = (float)((double)db.query_ssc_sum / (double)db.query_count);

    min_angle = db.query[0].angle_x10;
    max_angle = db.query[0].angle_x10;
    for (i = 1; i < db.query_count; i++)
    {
        if (db.query[i].angle_x10 < min_angle) min_angle = db.query[i].angle_x10;
        if (db.query[i].angle_x10 > max_angle) max_angle = db.query[i].angle_x10;
    }

    feature[0] = db_log10_safe(rms);
    feature[1] = db_log10_safe(mav);
    feature[2] = db_log10_safe(wl / n);
    feature[3] = mav > 1.0e-12f ? rms / mav : 0.0f;
    feature[4] = zc / n;
    feature[5] = ssc / n;
    feature[6] = (float)(max_angle - min_angle) / 10.0f;
    feature[7] = (float)db.query_active_count / n;

    for (i = 0; i < 8U; i++)
    {
        z[i] = (feature[i] - runtime_mean[i]) / runtime_std[i];
    }
    return RT_TRUE;
}

static rt_bool_t db_parse_runtime_line(char *line, fatigue_db_runtime_row_t *row)
{
    char *cursor;
    char *field;
    int column;

    if (line == RT_NULL || row == RT_NULL || line[0] == '\0') return RT_FALSE;
    if (rt_strncmp(line, "source_family", 13) == 0) return RT_FALSE;

    rt_memset(row, 0, sizeof(*row));
    cursor = line;
    for (column = 0; column < 39; column++)
    {
        field = db_csv_next(&cursor);
        if (field == RT_NULL) return RT_FALSE;

        switch (column)
        {
        case 0: db_copy(row->source_family, sizeof(row->source_family), field); break;
        case 4: db_copy(row->person_id, sizeof(row->person_id), field); break;
        case 6: db_copy(row->unit_id, sizeof(row->unit_id), field); break;
        case 8: db_copy(row->label, sizeof(row->label), field); break;
        case 9: db_copy(row->fatigue_level, sizeof(row->fatigue_level), field); break;
        default:
            if (column >= 31 && column <= 38)
                row->z[column - 31] = db_float(field);
            break;
        }
    }

    return row->source_family[0] != '\0' && row->unit_id[0] != '\0';
}

static rt_bool_t db_runtime_mode_accepts(const char *mode,
                                         const fatigue_db_runtime_row_t *row)
{
    if (mode == RT_NULL || row == RT_NULL) return RT_FALSE;
    if (rt_strcmp(mode, "mixed_shape") == 0 ||
        rt_strcmp(mode, "mixed") == 0 ||
        rt_strcmp(mode, "all") == 0)
        return RT_TRUE;
    if (rt_strcmp(mode, "device_core") == 0)
        return rt_strcmp(row->source_family, "local_logged") == 0 ||
               rt_strcmp(row->source_family, "builtin_profile") == 0;
    if (rt_strcmp(mode, "external_only") == 0)
        return rt_strcmp(row->source_family, "external_public") == 0;
    return RT_FALSE;
}

static rt_bool_t db_runtime_level_score(const char *level, uint16_t *score)
{
    if (level == RT_NULL || score == RT_NULL) return RT_FALSE;

    if (rt_strcmp(level, "none") == 0)
        *score = 10U;
    else if (rt_strcmp(level, "low") == 0)
        *score = 35U;
    else if (rt_strcmp(level, "medium") == 0)
        *score = 65U;
    else if (rt_strcmp(level, "high") == 0)
        *score = 90U;
    else
        return RT_FALSE;

    return RT_TRUE;
}

static void db_runtime_insert_candidate(fatigue_db_candidate_t candidates[],
                                        const fatigue_db_runtime_row_t *row,
                                        float distance)
{
    int pos;
    int i;

    if (candidates == RT_NULL || row == RT_NULL) return;

    for (pos = 0; pos < FATIGUE_DB_TOP_K; pos++)
    {
        if (!candidates[pos].valid || distance < candidates[pos].distance) break;
    }
    if (pos >= FATIGUE_DB_TOP_K) return;

    for (i = FATIGUE_DB_TOP_K - 1; i > pos; i--)
        candidates[i] = candidates[i - 1];

    candidates[pos].valid = RT_TRUE;
    candidates[pos].row = *row;
    candidates[pos].distance = distance;
}

static uint32_t db_runtime_distance_x1000(float distance)
{
    float scaled = distance * 1000.0f;

    if (scaled <= 0.0f) return 0U;
    if (scaled >= 4294967295.0f) return 0xFFFFFFFFU;
    return (uint32_t)(scaled + 0.5f);
}

static float db_runtime_row_distance(const float query_z[8],
                                     const fatigue_db_runtime_row_t *row)
{
    float weighted_sum = 0.0f;
    float weight_sum = 0.0f;
    int i;

    for (i = 0; i < 8; i++)
    {
        float delta = query_z[i] - row->z[i];
        weighted_sum += runtime_weight[i] * delta * delta;
        weight_sum += runtime_weight[i];
    }
    return weight_sum > 0.0f ? sqrtf(weighted_sum / weight_sum) : 999.0f;
}

static rt_err_t db_runtime_match_all(const char *requested_mode,
                                     fatigue_db_runtime_match_t *out)
{
    struct dfs_fd fd;
    char line[FATIGUE_DB_RUNTIME_LINE_MAX];
    fatigue_db_runtime_match_t best;
    static fatigue_db_candidate_t top[FATIGUE_DB_TOP_K];
    static fatigue_db_candidate_t known_top[FATIGUE_DB_TOP_K];
    float query_z[8];
    const char *mode = requested_mode;
    uint32_t rows_scanned = 0;
    uint32_t rows_eligible = 0;
    uint16_t top_count = 0;
    uint16_t known_count = 0;
    int k;

    if (mode == RT_NULL || mode[0] == '\0') mode = "mixed_shape";
    if (rt_strcmp(mode, "mixed_shape") != 0 &&
        rt_strcmp(mode, "mixed") != 0 &&
        rt_strcmp(mode, "all") != 0 &&
        rt_strcmp(mode, "device_core") != 0 &&
        rt_strcmp(mode, "external_only") != 0)
        return -RT_EINVAL;

    rt_mutex_take(db.lock, RT_WAITING_FOREVER);
    if (!db_runtime_build_query(query_z))
    {
        rt_mutex_release(db.lock);
        return -RT_EEMPTY;
    }

    rt_memset(&best, 0, sizeof(best));
    rt_memset(top, 0, sizeof(top));
    rt_memset(known_top, 0, sizeof(known_top));

    rt_memset(&fd, 0, sizeof(fd));
    if (db.ready && dfs_file_open(&fd, FATIGUE_DB_RUNTIME_UNITS, O_RDONLY) == RT_EOK)
    {
        while (db_read_line(&fd, line, sizeof(line)) > 0)
        {
            fatigue_db_runtime_row_t row;
            float distance;

            if (!db_parse_runtime_line(line, &row)) continue;
            rows_scanned++;
            if (!db_runtime_mode_accepts(mode, &row)) continue;
            rows_eligible++;

            distance = db_runtime_row_distance(query_z, &row);

            db_runtime_insert_candidate(top, &row, distance);
            {
                uint16_t ignored_score;
                if (db_runtime_level_score(row.fatigue_level, &ignored_score))
                    db_runtime_insert_candidate(known_top, &row, distance);
            }
        }
        dfs_file_close(&fd);
    }
    else if (rt_strcmp(mode, "external_only") != 0)
    {
        uint16_t p;

        rt_kprintf("DB,RUNTIME_BUILTIN,count=%u,path=%s\n",
                   (unsigned)FATIGUE_DB_BUILTIN_PROFILE_COUNT,
                   FATIGUE_DB_RUNTIME_UNITS);
        for (p = 0; p < FATIGUE_DB_BUILTIN_PROFILE_COUNT; p++)
        {
            fatigue_db_runtime_row_t row;
            float distance;
            int i;

            rt_memset(&row, 0, sizeof(row));
            db_copy(row.source_family, sizeof(row.source_family), "builtin_profile");
            db_copy(row.person_id, sizeof(row.person_id), builtin_profiles[p].person_id);
            rt_snprintf(row.unit_id, sizeof(row.unit_id), "profile_%02u", (unsigned)(p + 1U));
            db_copy(row.label, sizeof(row.label), "person_fatigue_type");
            db_copy(row.fatigue_level, sizeof(row.fatigue_level),
                    builtin_profiles[p].fatigue_level);
            for (i = 0; i < 8; i++) row.z[i] = builtin_profiles[p].z[i];

            rows_scanned++;
            rows_eligible++;
            distance = db_runtime_row_distance(query_z, &row);
            db_runtime_insert_candidate(top, &row, distance);
            db_runtime_insert_candidate(known_top, &row, distance);
        }
    }

    best.rows_scanned = rows_scanned;
    best.rows_eligible = rows_eligible;
    for (k = 0; k < FATIGUE_DB_TOP_K && top[k].valid; k++) top_count++;
    for (k = 0; k < FATIGUE_DB_TOP_K && known_top[k].valid; k++) known_count++;

    if (top_count > 0U)
    {
        best.matched = RT_TRUE;
        db_copy(best.mode, sizeof(best.mode), mode);
        db_copy(best.source_family, sizeof(best.source_family), top[0].row.source_family);
        db_copy(best.person_id, sizeof(best.person_id), top[0].row.person_id);
        db_copy(best.unit_id, sizeof(best.unit_id), top[0].row.unit_id);
        db_copy(best.label, sizeof(best.label), top[0].row.label);
        db_copy(best.fatigue_level, sizeof(best.fatigue_level), top[0].row.fatigue_level);
        best.distance_x1000 = db_runtime_distance_x1000(top[0].distance);
        best.top_k = top_count;
        best.known_votes = known_count;

        if (known_count > 0U)
        {
            float vote_sum = 0.0f;
            float vote_weight = 0.0f;
            float nearest_similarity;
            uint16_t min_score = 100U;
            uint16_t max_score = 0U;
            uint16_t coverage;
            uint16_t agreement;
            uint16_t confidence;

            for (k = 0; k < known_count; k++)
            {
                uint16_t level_score = 0U;
                float weight;

                if (!db_runtime_level_score(known_top[k].row.fatigue_level, &level_score))
                    continue;
                weight = 1.0f / (0.05f + known_top[k].distance);
                vote_sum += weight * (float)level_score;
                vote_weight += weight;
                if (level_score < min_score) min_score = level_score;
                if (level_score > max_score) max_score = level_score;
            }

            if (vote_weight > 0.0f)
                best.database_score = (uint16_t)(vote_sum / vote_weight + 0.5f);
            best.reference_distance_x1000 =
                db_runtime_distance_x1000(known_top[0].distance);

            nearest_similarity = 100.0f / (1.0f + known_top[0].distance);
            coverage = (uint16_t)(known_count * 100U / FATIGUE_DB_TOP_K);
            agreement = (uint16_t)(100U - (max_score - min_score));
            confidence = (uint16_t)(nearest_similarity * 0.50f +
                                    (float)coverage * 0.30f +
                                    (float)agreement * 0.20f + 0.5f);
            if (known_count == 1U && confidence > 65U) confidence = 65U;
            if (known_count == 2U && confidence > 80U) confidence = 80U;
            if (confidence > 100U) confidence = 100U;
            best.confidence = confidence;
        }

        for (k = 0; k < top_count; k++)
        {
            rt_kprintf("DB,TOPK,%d,distance_x1000=%lu,source=%s,person=%s,unit=%s,label=%s,fatigue=%s\n",
                       k + 1,
                       (unsigned long)db_runtime_distance_x1000(top[k].distance),
                       top[k].row.source_family,
                       top[k].row.person_id,
                       top[k].row.unit_id,
                       top[k].row.label,
                       top[k].row.fatigue_level);
        }

        db.runtime_match = best;
        db_publish_runtime_match(&best);
        db.runtime_last_match_tick = rt_tick_get();
        if (out != RT_NULL) *out = best;
    }
    else
    {
        rt_memset(&db.runtime_match, 0, sizeof(db.runtime_match));
        db_publish_runtime_match(&db.runtime_match);
    }

    rt_mutex_release(db.lock);
    return best.matched ? RT_EOK : -RT_ERROR;
}

static void db_prepare_if_possible(void)
{
    rt_mutex_take(db.lock, RT_WAITING_FOREVER);

    if (!db.ready)
    {
        if (db_prepare_files() == RT_EOK)
        {
            db.ready = RT_TRUE;
            db.session_open = RT_TRUE;
            db_write_session_event("start", "auto");
            rt_kprintf("DB,READY,%s\n", FATIGUE_DB_ROOT);
        }
        else
        {
            db_close_all();
        }
    }

    rt_mutex_release(db.lock);
}

static void fatigue_db_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        rt_bool_t should_match = RT_FALSE;
        rt_tick_t now = rt_tick_get();

        if (!db.ready)
        {
            db_prepare_if_possible();
        }

        rt_mutex_take(db.lock, RT_WAITING_FOREVER);
        if (db.query_count >= FATIGUE_DB_QUERY_ROWS &&
            db.query_active_count >= FATIGUE_DB_QUERY_MIN_ACTIVE_ROWS &&
            (db.runtime_last_match_tick == 0 ||
             (rt_int32_t)(now - db.runtime_last_match_tick) >=
                 (rt_int32_t)rt_tick_from_millisecond(FATIGUE_DB_MATCH_REFRESH_MS)))
        {
            should_match = RT_TRUE;
            db.runtime_last_match_tick = now;
        }
        rt_mutex_release(db.lock);

        if (should_match)
        {
            fatigue_db_runtime_match_t match;
            if (db_runtime_match_all("device_core", &match) == RT_EOK)
            {
                rt_kprintf("DB,RUNTIME_AUTO,rows=%lu,eligible=%lu,distance_x1000=%lu,"
                           "source=%s,person=%s,unit=%s,label=%s,fatigue=%s,"
                           "top_k=%u,known=%u,db_score=%u,confidence=%u\n",
                           (unsigned long)match.rows_scanned,
                           (unsigned long)match.rows_eligible,
                           (unsigned long)match.distance_x1000,
                           match.source_family,
                           match.person_id,
                           match.unit_id,
                           match.label,
                           match.fatigue_level,
                           (unsigned)match.top_k,
                           (unsigned)match.known_votes,
                           (unsigned)match.database_score,
                           (unsigned)match.confidence);
            }
        }
        rt_thread_mdelay(FATIGUE_DB_RETRY_MS);
    }
}

void fatigue_db_flush(void)
{
    rt_mutex_take(db.lock, RT_WAITING_FOREVER);
    if (users_open)    dfs_file_flush(&users_fd);
    if (sessions_open) dfs_file_flush(&sessions_fd);
    if (live_open)     dfs_file_flush(&live_fd);
    if (reps_open)     dfs_file_flush(&reps_fd);
    rt_mutex_release(db.lock);
}

void fatigue_db_log_live(const fatigue_db_live_record_t *rec)
{
    char line[384];

    if (rec == RT_NULL || db.lock == RT_NULL) return;

    rt_mutex_take(db.lock, RT_WAITING_FOREVER);
    if (rec->emg_phase != RT_NULL && rt_strcmp(rec->emg_phase, "RUN") == 0)
        db_query_push(rec);
    if (!db.ready || !live_open || !db.session_open)
    {
        rt_mutex_release(db.lock);
        return;
    }

    db.live_count++;
    if ((db.live_count % FATIGUE_DB_LIVE_DIV) != 0U)
    {
        rt_mutex_release(db.lock);
        return;
    }

    rt_snprintf(line, sizeof(line),
                "%lu,%s,%lu,%ld,%lu,%lu,%lu,%u,%u,%u,%u,%u,%u,%s,%s,%s,%ld,%d,%d,%d,%d,%d,%d\r\n",
                (unsigned long)db.session_id,
                db.user_id,
                (unsigned long)rec->t_ms,
                (long)rec->ac,
                (unsigned long)rec->rms,
                (unsigned long)rec->mav,
                (unsigned long)rec->wl,
                (unsigned)rec->zc,
                (unsigned)rec->ssc,
                (unsigned)rec->zcr_x1000,
                (unsigned)rec->fatigue_score,
                (unsigned)rec->fatigue_alert,
                (unsigned)rec->active,
                rec->emg_phase ? rec->emg_phase : "",
                rec->emg_status ? rec->emg_status : "",
                rec->motion_phase ? rec->motion_phase : "",
                (long)rec->angle_x10,
                rec->ax, rec->ay, rec->az, rec->gx, rec->gy, rec->gz);
    db_write(&live_fd, line);
    if ((db.live_count % 25U) == 0U) dfs_file_flush(&live_fd);
    rt_mutex_release(db.lock);
}

void fatigue_db_log_rep(const fatigue_db_rep_record_t *rec)
{
    char line[384];

    if (rec == RT_NULL || db.lock == RT_NULL) return;

    rt_mutex_take(db.lock, RT_WAITING_FOREVER);
    if (!db.ready || !reps_open || !db.session_open)
    {
        rt_mutex_release(db.lock);
        return;
    }

    rt_snprintf(line, sizeof(line),
                "%lu,%s,%lu,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%s,0x%02x,%lu\r\n",
                (unsigned long)db.session_id,
                db.user_id,
                (unsigned long)rec->rep_n,
                rec->action ? rec->action : "",
                (unsigned long)rec->depth_x10,
                (unsigned long)rec->descend_ms,
                (unsigned long)rec->bottom_ms,
                (unsigned long)rec->ascend_ms,
                (unsigned long)rec->iemg,
                (unsigned long)rec->mean_mav,
                (unsigned long)rec->mean_rms,
                (unsigned long)rec->mean_wl,
                (unsigned long)rec->peak_rms,
                (unsigned long)rec->peak_fatigue,
                (unsigned long)rec->quality_score,
                rec->quality_label ? rec->quality_label : "",
                (unsigned)rec->fail_mask,
                (unsigned long)rec->base_mav);
    db_write(&reps_fd, line);
    dfs_file_flush(&reps_fd);
    rt_mutex_release(db.lock);
}

void fatigue_db_start_session(const char *action_hint)
{
    rt_mutex_take(db.lock, RT_WAITING_FOREVER);

    if (action_hint != RT_NULL && action_hint[0] != '\0')
    {
        rt_strncpy(db.action_hint, action_hint, sizeof(db.action_hint) - 1U);
        db.action_hint[sizeof(db.action_hint) - 1U] = '\0';
    }

    db.session_id = rt_tick_get_millisecond();
    db.live_count = 0;
    db_query_reset();
    db.session_open = RT_TRUE;
    if (db.ready) db_write_session_event("start", "manual");

    rt_mutex_release(db.lock);
}

void fatigue_db_stop_session(void)
{
    rt_mutex_take(db.lock, RT_WAITING_FOREVER);
    if (db.ready && db.session_open)
    {
        db_write_session_event("stop", "manual");
        if (users_open)    dfs_file_flush(&users_fd);
        if (sessions_open) dfs_file_flush(&sessions_fd);
        if (live_open)     dfs_file_flush(&live_fd);
        if (reps_open)     dfs_file_flush(&reps_fd);
    }
    db.session_open = RT_FALSE;
    rt_mutex_release(db.lock);
}

void fatigue_db_set_user(const char *user_id)
{
    rt_mutex_take(db.lock, RT_WAITING_FOREVER);
    if (user_id != RT_NULL && user_id[0] != '\0')
    {
        rt_strncpy(db.user_id, user_id, sizeof(db.user_id) - 1U);
        db.user_id[sizeof(db.user_id) - 1U] = '\0';
    }
    rt_mutex_release(db.lock);
}

static void db_append_user_meta(int argc, char **argv)
{
    char line[256];

    if (argc >= 2) fatigue_db_set_user(argv[1]);

    rt_mutex_take(db.lock, RT_WAITING_FOREVER);
    if (argc > 2) db_copy(db.sex, sizeof(db.sex), argv[2]);
    if (argc > 3) db.age = (uint16_t)db_u32(argv[3]);
    if (argc > 4) db.height_cm = (uint16_t)db_u32(argv[4]);
    if (argc > 5) db.weight_kg = (uint16_t)db_u32(argv[5]);
    if (argc > 6) db.leg_cm = (uint16_t)db_u32(argv[6]);
    if (argc > 7) db_copy(db.habit, sizeof(db.habit), argv[7]);
    if (argc > 8) db_copy(db.injury, sizeof(db.injury), argv[8]);

    if (!db.ready || !users_open)
    {
        rt_mutex_release(db.lock);
        rt_kprintf("DB,NOT_READY\n");
        return;
    }

    rt_snprintf(line, sizeof(line), "%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s\r\n",
                (unsigned long)rt_tick_get_millisecond(),
                db.user_id,
                argc > 2 ? argv[2] : "",
                argc > 3 ? argv[3] : "",
                argc > 4 ? argv[4] : "",
                argc > 5 ? argv[5] : "",
                argc > 6 ? argv[6] : "",
                argc > 7 ? argv[7] : "",
                argc > 8 ? argv[8] : "",
                argc > 9 ? argv[9] : "");
    db_write(&users_fd, line);
    dfs_file_flush(&users_fd);
    rt_mutex_release(db.lock);
    rt_kprintf("DB,USER,%s\n", fatigue_db_user());
}

#else

static void fatigue_db_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
    rt_kprintf("DB,DISABLED,enable RT_USING_DFS and RT_USING_DFS_ELMFAT\n");
}

void fatigue_db_flush(void) {}
void fatigue_db_log_live(const fatigue_db_live_record_t *rec) { RT_UNUSED(rec); }
void fatigue_db_log_rep(const fatigue_db_rep_record_t *rec) { RT_UNUSED(rec); }
rt_err_t fatigue_db_match_current(const char *action_hint, fatigue_db_match_t *out)
{
    RT_UNUSED(action_hint);
    if (out != RT_NULL) rt_memset(out, 0, sizeof(*out));
    return -RT_ERROR;
}
static rt_err_t db_runtime_match_all(const char *requested_mode,
                                     fatigue_db_runtime_match_t *out)
{
    RT_UNUSED(requested_mode);
    if (out != RT_NULL) rt_memset(out, 0, sizeof(*out));
    return -RT_ERROR;
}
void fatigue_db_start_session(const char *action_hint) { RT_UNUSED(action_hint); }
void fatigue_db_stop_session(void) {}
void fatigue_db_set_user(const char *user_id)
{
    if (user_id != RT_NULL && user_id[0] != '\0')
    {
        rt_strncpy(db.user_id, user_id, sizeof(db.user_id) - 1U);
        db.user_id[sizeof(db.user_id) - 1U] = '\0';
    }
}

static void db_append_user_meta(int argc, char **argv)
{
    if (argc >= 2) fatigue_db_set_user(argv[1]);
    rt_kprintf("DB,DISABLED\n");
}

#endif

void fatigue_db_init(void)
{
    if (db.lock != RT_NULL) return;

    rt_memset(&db, 0, sizeof(db));
    rt_memset(&published_runtime_match, 0, sizeof(published_runtime_match));
    rt_strncpy(db.user_id, "unknown", sizeof(db.user_id) - 1U);
    rt_strncpy(db.action_hint, "mixed", sizeof(db.action_hint) - 1U);
    db.session_id = rt_tick_get_millisecond();
    db.session_open = RT_TRUE;

    db.lock = rt_mutex_create("fatdb", RT_IPC_FLAG_PRIO);
    if (db.lock == RT_NULL)
    {
        rt_kprintf("DB,ERR,mutex\n");
        return;
    }

    db.thread = rt_thread_create("fatdb",
                                 fatigue_db_thread_entry,
                                 RT_NULL,
                                 FATIGUE_DB_THREAD_STACK,
                                 FATIGUE_DB_THREAD_PRIO,
                                 10);
    if (db.thread != RT_NULL)
    {
        rt_thread_startup(db.thread);
    }
    else
    {
        rt_kprintf("DB,ERR,thread\n");
    }
}

rt_bool_t fatigue_db_ready(void)
{
    return db.ready;
}

const char *fatigue_db_user(void)
{
    return db.user_id;
}

void fatigue_db_get_match(fatigue_db_match_t *out)
{
    if (out == RT_NULL) return;

    if (db.lock != RT_NULL)
    {
        rt_mutex_take(db.lock, RT_WAITING_FOREVER);
        *out = db.match;
        rt_mutex_release(db.lock);
    }
    else
    {
        rt_memset(out, 0, sizeof(*out));
    }
}

void fatigue_db_get_runtime_match(fatigue_db_runtime_match_t *out)
{
    if (out == RT_NULL) return;

    rt_enter_critical();
    *out = published_runtime_match;
    rt_exit_critical();
}

uint16_t fatigue_db_fuse_score(uint16_t model_score, rt_bool_t *used)
{
    fatigue_db_runtime_match_t match;
    uint32_t db_weight;
    uint32_t fused;

    if (used != RT_NULL) *used = RT_FALSE;
    if (model_score > 100U) model_score = 100U;

    fatigue_db_get_runtime_match(&match);
    if (!match.matched || match.known_votes < 3U || match.confidence == 0U)
        return model_score;

    db_weight = ((uint32_t)match.confidence * FATIGUE_DB_MAX_BLEND_PERCENT + 50U) / 100U;
    if (db_weight == 0U) return model_score;

    fused = ((uint32_t)model_score * (100U - db_weight) +
             (uint32_t)match.database_score * db_weight + 50U) / 100U;
    if (fused > 100U) fused = 100U;
    if (used != RT_NULL) *used = RT_TRUE;
    return (uint16_t)fused;
}

#ifdef FINSH_USING_MSH
static void db_status(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    rt_kprintf("DB,status=%s,user=%s,age=%u,height=%u,weight=%u,leg=%u,session=%lu,path=%s\n",
               db.ready ? "READY" : "WAIT_SD",
               db.user_id,
               (unsigned)db.age,
               (unsigned)db.height_cm,
               (unsigned)db.weight_kg,
               (unsigned)db.leg_cm,
               (unsigned long)db.session_id,
               FATIGUE_DB_ROOT);
    rt_kprintf("DB,runtime=%s,query_rows=%u,last_scan=%lu,last_eligible=%lu\n",
               FATIGUE_DB_RUNTIME_UNITS,
               (unsigned)db.query_count,
               (unsigned long)db.runtime_match.rows_scanned,
               (unsigned long)db.runtime_match.rows_eligible);
    if (db.runtime_match.matched)
    {
        rt_kprintf("DB,runtime_match=%s,unit=%s,distance_x1000=%lu,top_k=%u,known=%u,db_score=%u,confidence=%u\n",
                   db.runtime_match.person_id,
                   db.runtime_match.unit_id,
                   (unsigned long)db.runtime_match.distance_x1000,
                   (unsigned)db.runtime_match.top_k,
                   (unsigned)db.runtime_match.known_votes,
                   (unsigned)db.runtime_match.database_score,
                   (unsigned)db.runtime_match.confidence);
    }
    if (db.match.matched)
    {
        rt_kprintf("DB,match=%s,score=%lu,action=%s,light=%u,mid=%u,heavy=%u\n",
                   db.match.person_id,
                   (unsigned long)db.match.score,
                   db.match.action,
                   (unsigned)db.match.fatigue_light,
                   (unsigned)db.match.fatigue_mid,
                   (unsigned)db.match.fatigue_heavy);
    }
}
MSH_CMD_EXPORT(db_status, show fatigue SD database status);

static void db_user(int argc, char **argv)
{
    db_append_user_meta(argc, argv);
    rt_kprintf("usage: db_user id sex age height_cm weight_kg leg_cm habit injury note\n");
}
MSH_CMD_EXPORT(db_user, set current user and append user metadata);

static void db_session_start(int argc, char **argv)
{
    fatigue_db_start_session(argc >= 2 ? argv[1] : "mixed");
    rt_kprintf("DB,SESSION_START,%lu,%s\n",
               (unsigned long)db.session_id,
               db.action_hint);
}
MSH_CMD_EXPORT(db_session_start, start a new fatigue database session);

static void db_session_stop(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    fatigue_db_stop_session();
    rt_kprintf("DB,SESSION_STOP\n");
}
MSH_CMD_EXPORT(db_session_stop, stop current fatigue database session);

static void db_flush(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    fatigue_db_flush();
    rt_kprintf("DB,FLUSH\n");
}
MSH_CMD_EXPORT(db_flush, flush fatigue database files);

static void db_match(int argc, char **argv)
{
    fatigue_db_match_t m;
    const char *action = argc >= 2 ? argv[1] : RT_NULL;
    rt_err_t result = fatigue_db_match_current(action, &m);

    if (result != RT_EOK)
    {
        rt_kprintf("DB,MATCH_FAIL,put %s on SD card and run after EMG calibration\n",
                   FATIGUE_DB_REFS);
    }
}
MSH_CMD_EXPORT(db_match, match current user to compatible reference profiles);

static void db_runtime_match(int argc, char **argv)
{
    fatigue_db_runtime_match_t match;
    const char *mode = argc >= 2 ? argv[1] : "mixed_shape";
    rt_err_t result = db_runtime_match_all(mode, &match);

    if (result != RT_EOK)
    {
        rt_kprintf("DB,RUNTIME_MATCH_FAIL,path=%s,query_rows=%u\n",
                   FATIGUE_DB_RUNTIME_UNITS,
                   (unsigned)db.query_count);
        rt_kprintf("usage: db_runtime_match mixed_shape|device_core|external_only\n");
        return;
    }

    rt_kprintf("DB,RUNTIME_MATCH,mode=%s,rows=%lu,eligible=%lu,distance_x1000=%lu,"
               "source=%s,person=%s,unit=%s,label=%s,fatigue=%s,"
               "top_k=%u,known=%u,db_score=%u,confidence=%u\n",
               match.mode,
               (unsigned long)match.rows_scanned,
               (unsigned long)match.rows_eligible,
               (unsigned long)match.distance_x1000,
               match.source_family,
               match.person_id,
               match.unit_id,
               match.label,
               match.fatigue_level,
               (unsigned)match.top_k,
               (unsigned)match.known_votes,
               (unsigned)match.database_score,
               (unsigned)match.confidence);
}
MSH_CMD_EXPORT(db_runtime_match, scan all 1734 normalized runtime samples);

static void db_match_status(int argc, char **argv)
{
    fatigue_db_match_t m;
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    fatigue_db_get_match(&m);
    if (!m.matched)
    {
        rt_kprintf("DB,MATCH,NONE\n");
        return;
    }

    rt_kprintf("DB,MATCH,%s,score=%lu,action=%s,ref_rms=%lu,ref_mav=%lu,light=%u,mid=%u,heavy=%u\n",
               m.person_id,
               (unsigned long)m.score,
               m.action,
               (unsigned long)m.base_rms,
               (unsigned long)m.base_mav,
               (unsigned)m.fatigue_light,
               (unsigned)m.fatigue_mid,
               (unsigned)m.fatigue_heavy);
}
MSH_CMD_EXPORT(db_match_status, show current matched reference person);
#endif
