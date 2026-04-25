#pragma once
#include <stdint.h>

typedef enum{
    CACHE_ASSETS_AK_BAR = 0,
    CACHE_ASSETS_BTM_LEFT_BAR,
    CACHE_ASSETS_TOP_LEFT_RECT,
    CACHE_ASSETS_TOP_LEFT_RHODES,
    CACHE_ASSETS_TOP_RIGHT_BAR,
    CACHE_ASSETS_TOP_RIGHT_ARROW,


    CACHE_ASSETS_MAX_ASSET_MAX,
} cacheasset_asset_id_t;

typedef struct{
    uint8_t* base_addr;
    int curr_size;
    int max_size;
    uint8_t* asset_addr[CACHE_ASSETS_MAX_ASSET_MAX];
    int asset_w[CACHE_ASSETS_MAX_ASSET_MAX];
    int asset_h[CACHE_ASSETS_MAX_ASSET_MAX];

} cacheassets_t;

void cacheassets_init(cacheassets_t* cacheassets,uint8_t* base_addr,int max_size);
void cacheassets_put_asset(cacheassets_t* cacheassets,cacheasset_asset_id_t asset_id,char* image_path);
void cacheassets_get_asset(cacheassets_t* cacheassets,cacheasset_asset_id_t asset_id,int* w,int* h,uint8_t** pixdata);
void cacheassets_get_asset_from_global(cacheasset_asset_id_t asset_id,int* w,int* h,uint8_t** pixdata);
