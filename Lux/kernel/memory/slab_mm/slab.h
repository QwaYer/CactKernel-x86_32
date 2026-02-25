#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Slab-аллокатор для мелких структур ядра
 *
 * Архитектура:
 *   slab_cache_t  — кэш для объектов одного фиксированного размера.
 *                   Содержит три списка slab'ов: full / partial / free.
 *   slab_t        — один физический блок (одна страница 4 KiB).
 *                   В начале страницы расположен заголовок slab_t,
 *                   за ним — массив объектов.
 *
 *  Схема одной страницы (PAGE_SIZE = 4096):
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │  slab_t header  │  obj[0]  │  obj[1]  │ ... │  obj[N-1]    │
 *   └──────────────────────────────────────────────────────────────┘
 *
 *  Свободные объекты связаны через freelist — первые 4 байта объекта
 *  используются как указатель на следующий свободный (union trick).
 * ----------------------------------------------------------------------- */

#define SLAB_MAX_OBJ_SIZE  2048   /* максимальный размер одного объекта      */
#define SLAB_MIN_OBJ_SIZE  8      /* минимальный полезный размер             */
#define SLAB_NAME_LEN      32     /* длина имени кэша                        */

/* Один slab = одна страница памяти */
typedef struct slab {
    struct slab*  next;       /* следующий slab в списке                     */
    struct slab*  prev;       /* предыдущий slab в списке                    */
    void*         freelist;   /* голова списка свободных объектов            */
    uint32_t      inuse;      /* количество занятых объектов                 */
    uint32_t      capacity;   /* всего объектов в slab'е                     */
    struct slab_cache* cache; /* обратная ссылка на кэш                      */
    /* объекты идут сразу за заголовком в той же странице */
} slab_t;

/* Кэш объектов одного типа */
typedef struct slab_cache {
    char     name[SLAB_NAME_LEN]; /* отладочное имя ("task_struct" и т.п.)  */
    uint32_t obj_size;            /* размер одного объекта (выровнен до 8)   */
    uint32_t objs_per_slab;       /* объектов помещается в одну страницу     */

    slab_t*  partial;             /* slabs с частично занятыми объектами     */
    slab_t*  full;                /* slabs, где нет свободных мест           */
    slab_t*  free;                /* полностью пустые slabs (резерв)         */

    uint32_t total_slabs;         /* всего slab'ов выделено                  */
    uint32_t total_allocs;        /* счётчик аллокаций (для отладки)         */
    uint32_t total_frees;         /* счётчик освобождений                    */

    /* Опциональные конструктор / деструктор объекта */
    void (*ctor)(void* obj);
    void (*dtor)(void* obj);

    struct slab_cache* next;      /* глобальный список всех кэшей            */
} slab_cache_t;

/* ----------------------------------------------------------------------- */
/*  Public API                                                               */
/* ----------------------------------------------------------------------- */

/**
 * slab_cache_create — создать новый кэш объектов.
 *
 * @name      имя для отладки
 * @obj_size  размер одного объекта в байтах
 * @ctor      конструктор (может быть NULL)
 * @dtor      деструктор  (может быть NULL)
 * @return    указатель на новый slab_cache_t, или NULL при ошибке
 */
slab_cache_t* slab_cache_create(const char* name,
                                uint32_t    obj_size,
                                void      (*ctor)(void*),
                                void      (*dtor)(void*));

/**
 * slab_alloc — выделить один объект из кэша.
 *
 * @return указатель на объект, или NULL если памяти нет.
 */
void* slab_alloc(slab_cache_t* cache);

/**
 * slab_free — вернуть объект в кэш.
 */
void  slab_free(slab_cache_t* cache, void* obj);

/**
 * slab_cache_destroy — уничтожить кэш и вернуть все страницы в PMM.
 * Все объекты должны быть освобождены до вызова.
 */
void  slab_cache_destroy(slab_cache_t* cache);

/**
 * slab_cache_shrink — вернуть в PMM все полностью пустые slabs.
 */
void  slab_cache_shrink(slab_cache_t* cache);

/**
 * slab_print_stats — вывести статистику кэша через kprint.
 */
void  slab_print_stats(const slab_cache_t* cache);

/**
 * slab_init — инициализировать подсистему (вызвать после init_heap).
 */
void  slab_init(void);

/* ----------------------------------------------------------------------- */
/*  Предопределённые кэши общего назначения (аналог kmalloc-кэшей в Linux) */
/* ----------------------------------------------------------------------- */
void* slab_kmalloc(uint32_t size);   /* thin wrapper вокруг slab_alloc      */
void  slab_kfree(void* ptr);         /* thin wrapper вокруг slab_free        */

#endif /* SLAB_H */