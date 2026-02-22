/*
 * fuse_xattr.c - OBMAFS3 FUSE extended attribute operations
 *
 * Implements: getxattr, setxattr, listxattr, removexattr
 */

#include "fuse_ops_internal.h"
#include "debug.h"
#include "tags.h"

/* ------------------------------------------------------------------ */
/*  Media tag xattr helpers                                            */
/* ------------------------------------------------------------------ */

#define MEDIATAG_XATTR_PREFIX     "user.mediatag."
#define MEDIATAG_XATTR_PREFIX_LEN 14 /* strlen("user.mediatag.") */

/* Filesystem identity xattr — exposed only on the root directory */
#define FSTYPE_XATTR_NAME  "system.obmafs3.fstype"
#define FSTYPE_XATTR_VALUE "obmafs3"

/** Map MediaTagType ordinal to xattr suffix name.  NULL = unused slot. */
static const char *media_tag_xattr_names[] = {
    [kCdTableOfContents]             = "cd_toc",
    [kCdSessionInfo]                 = "cd_session_info",
    [kCdFullTOC]                     = "cd_full_toc",
    [kCdPMA]                         = "cd_pma",
    [kCdATIP]                        = "cd_atip",
    [kCdTEXT]                        = "cd_text",
    [kCdMCN]                         = "cd_mcn",
    [kDvdPFI]                        = "dvd_pfi",
    [kDvdCMI]                        = "dvd_cmi",
    [kDvdDiscKey]                    = "dvd_disc_key",
    [kDvdBCA]                        = "dvd_bca",
    [kDvdDMI]                        = "dvd_dmi",
    [kDvdMediaIdentifier]            = "dvd_media_id",
    [kDvdMKB]                        = "dvd_mkb",
    [kDvdRamDDS]                     = "dvd_ram_dds",
    [kDvdRamMediumStatus]            = "dvd_ram_medium_status",
    [kDvdRamSpareArea]               = "dvd_ram_spare_area",
    [kDvdRecordableRMD]              = "dvd_recordable_rmd",
    [kDvdRecordablePreRecordedInfo]  = "dvd_recordable_pre_recorded_info",
    [kDvdRecordableMediaIdentifier]  = "dvd_recordable_media_id",
    [kDvdRecordablePFI]              = "dvd_recordable_pfi",
    [kDvdADIP]                       = "dvd_adip",
    [kHdDvdCPI]                      = "hddvd_cpi",
    [kHdDvdMediumStatus]             = "hddvd_medium_status",
    [kDvdDlLayerCapacity]            = "dvd_dl_layer_capacity",
    [kDvdDlMiddleZoneAddress]        = "dvd_dl_middle_zone_address",
    [kDvdDlJumpIntervalSize]         = "dvd_dl_jump_interval_size",
    [kDvdDlManualLayerJumpLBA]       = "dvd_dl_manual_layer_jump_lba",
    [kBdDI]                          = "bd_di",
    [kBdBCA]                         = "bd_bca",
    [kBdDDS]                         = "bd_dds",
    [kBdCartridgeStatus]             = "bd_cartridge_status",
    [kBdSpareArea]                   = "bd_spare_area",
    [kAACS_VolumeIdentifier]         = "aacs_volume_id",
    [kAACS_SerialNumber]             = "aacs_serial_number",
    [kAACS_MediaIdentifier]          = "aacs_media_id",
    [kAACS_MKB]                      = "aacs_mkb",
    [kAACS_DataKeys]                 = "aacs_data_keys",
    [kAACS_LBAExtents]               = "aacs_lba_extents",
    [kAACS_CPRM_MKB]                 = "aacs_cprm_mkb",
    [kHybrid_RecognizedLayers]       = "hybrid_recognized_layers",
    [kMMC_WriteProtection]           = "mmc_write_protection",
    [kMMC_DiscInformation]           = "mmc_disc_information",
    [kMMC_TrackResourcesInformation] = "mmc_track_resources",
    [kMMC_POWResourcesInformation]   = "mmc_pow_resources",
    [kSCSI_INQUIRY]                  = "scsi_inquiry",
    [kSCSI_MODEPAGE_2A]              = "scsi_modepage_2a",
    [kATA_IDENTIFY]                  = "ata_identify",
    [kATAPI_IDENTIFY]                = "atapi_identify",
    [kPCMCIA_CIS]                    = "pcmcia_cis",
    [kSecureDigital_CID]             = "sd_cid",
    [kSecureDigital_CSD]             = "sd_csd",
    [kSecureDigital_SCR]             = "sd_scr",
    [kSecureDigital_OCR]             = "sd_ocr",
    [kMMC_CID]                       = "mmc_cid",
    [kMMC_CSD]                       = "mmc_csd",
    [kMMC_OCR]                       = "mmc_ocr",
    [kMMC_ExtendedCSD]               = "mmc_extended_csd",
    [kXbox_SecuritySector]           = "xbox_security_sector",
    [kFloppy_LeadOut]                = "floppy_lead_out",
    [kDiscControlBlock]              = "disc_control_block",
    [kCD_FirstTrackPregap]           = "cd_first_track_pregap",
    [kCD_LeadOut]                    = "cd_lead_out",
    [kSCSI_MODESENSE_6]              = "scsi_mode_sense_6",
    [kSCSI_MODESENSE_10]             = "scsi_mode_sense_10",
    [kUSB_Descriptors]               = "usb_descriptors",
    [kXbox_DMI]                      = "xbox_dmi",
    [kXbox_PFI]                      = "xbox_pfi",
    [kMiniDiscType]                  = "minidisc_type",
    [kMiniDiscD5]                    = "minidisc_d5",
    [kMiniDiscUTOC]                  = "minidisc_utoc",
    [kMiniDiscDTOC]                  = "minidisc_dtoc",
    [kDVD_DiscKey_Decrypted]         = "dvd_disc_key_decrypted",
    [kDVD_PFI_2ndLayer]              = "dvd_pfi_2nd_layer",
    [kFloppy_WriteProtect]           = "floppy_write_protect",
};

#define MEDIA_TAG_XATTR_COUNT (sizeof(media_tag_xattr_names) / sizeof(media_tag_xattr_names[0]))

/** Parse "user.mediatag.<name>" and return the tag type, or -1 on error. */
static int parse_mediatag_xattr(const char *name)
{
    if(strncmp(name, MEDIATAG_XATTR_PREFIX, MEDIATAG_XATTR_PREFIX_LEN) != 0) return -1;

    const char *tag_name = name + MEDIATAG_XATTR_PREFIX_LEN;

    for(size_t i = 0; i < MEDIA_TAG_XATTR_COUNT; i++)
    {
        if(media_tag_xattr_names[i] && strcmp(tag_name, media_tag_xattr_names[i]) == 0) return (int)i;
    }

    return -1;
}

/** Check whether a name starts with the mediatag xattr prefix. */
static int is_mediatag_xattr(const char *name)
{
    return strncmp(name, MEDIATAG_XATTR_PREFIX, MEDIATAG_XATTR_PREFIX_LEN) == 0;
}

#define METADATA_XATTR_PREFIX     "user.metadata."
#define METADATA_XATTR_PREFIX_LEN 14 /* strlen("user.metadata.") */

/**
 * Check whether @p name starts with the metadata xattr prefix
 * (@c "user.metadata.").
 *
 * @param name  Extended attribute name.
 * @return Non-zero if @p name is a metadata xattr, zero otherwise.
 */
static int is_metadata_xattr(const char *name)
{
    return strncmp(name, METADATA_XATTR_PREFIX, METADATA_XATTR_PREFIX_LEN) == 0;
}

/** Return true if the file type is a disk or CD image. */
static int is_image_file_type(uint8_t ft) { return ft == kFileTypeMediaImage || ft == kFileTypeCompactDiscImage; }

/* ------------------------------------------------------------------ */
/*  xattr FUSE callbacks                                               */
/* ------------------------------------------------------------------ */

/**
 * FUSE callback: retrieve an extended attribute value.
 *
 * Handles both media tag xattrs (@c "user.mediatag.*") and metadata
 * xattrs (@c "user.metadata.*").  Returns @c -ENODATA if the
 * attribute is not found or the file is not a media image.
 */
static int obmafs3_fuse_getxattr_impl(const char *path, const char *name, char *value, size_t size)
{
    /* Root-only system xattr for filesystem identification */
    if(strcmp(path, "/") == 0 && strcmp(name, FSTYPE_XATTR_NAME) == 0)
    {
        size_t vlen = strlen(FSTYPE_XATTR_VALUE);
        if(size == 0) return (int)vlen;
        if(size < vlen) FUSE_RETURN(-ERANGE, "");
        memcpy(value, FSTYPE_XATTR_VALUE, vlen);
        return (int)vlen;
    }

    int want_mediatag = is_mediatag_xattr(name);
    int want_metadata = is_metadata_xattr(name);

    if(!want_mediatag && !want_metadata) return -ENODATA;

    if(strcmp(path, "/") == 0) return -ENODATA;

    uint64_t    parent_id;
    const char *fname;
    int         rc = resolve_path(path, &parent_id, &fname);
    if(rc != 0) return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, fname, &cat_entry);
    if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENOENT, "");
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    if(!is_image_file_type(inode.file_type)) return -ENODATA;

    if(want_mediatag)
    {
        int tag_type = parse_mediatag_xattr(name);
        if(tag_type < 0) return -ENODATA;

        void    *data;
        uint32_t data_length;
        rc = obmafs3_media_tag_get(g_ctx, cat_entry.inode_id, (uint16_t)tag_type, &data, &data_length);
        if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENODATA, "");
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

        if(size == 0)
        {
            obmafs3_media_tag_data_free(data);
            return (int)data_length;
        }
        if(size < data_length)
        {
            obmafs3_media_tag_data_free(data);
            FUSE_RETURN(-ERANGE, "");
        }
        memcpy(value, data, data_length);
        obmafs3_media_tag_data_free(data);
        return (int)data_length;
    }

    /* user.metadata.<key> */
    const char *meta_key = name + METADATA_XATTR_PREFIX_LEN;
    if(*meta_key == '\0') return -ENODATA;

    char meta_value[METADATA_VALUE_MAX];
    rc = obmafs3_metadata_get(g_ctx, cat_entry.inode_id, meta_key, meta_value, sizeof(meta_value));
    if(rc == OBMAFS3_ERR_NOTFOUND) return -ENODATA;
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    size_t vlen = strlen(meta_value);
    if(size == 0) return (int)vlen;
    if(size < vlen) FUSE_RETURN(-ERANGE, "");
    memcpy(value, meta_value, vlen);
    return (int)vlen;
}

int obmafs3_fuse_getxattr(const char *path, const char *name, char *value, size_t size)
{
    pthread_rwlock_rdlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_getxattr_impl(path, name, value, size);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}

/**
 * FUSE callback: set an extended attribute.
 *
 * Stores media tag data or metadata key/value pairs on media image
 * files.  Returns @c -ENOTSUP for non-image files or unrecognised
 * attribute namespaces.
 */
static int obmafs3_fuse_setxattr_impl(const char *path, const char *name, const char *value, size_t size, int flags)
{
    (void)flags;

    int want_mediatag = is_mediatag_xattr(name);
    int want_metadata = is_metadata_xattr(name);

    if(!want_mediatag && !want_metadata) return -ENOTSUP;

    if(strcmp(path, "/") == 0) return -ENOTSUP;

    uint64_t    parent_id;
    const char *fname;
    int         rc = resolve_path(path, &parent_id, &fname);
    if(rc != 0) return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, fname, &cat_entry);
    if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENOENT, "");
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    if(!is_image_file_type(inode.file_type)) return -ENOTSUP;

    if(want_mediatag)
    {
        int tag_type = parse_mediatag_xattr(name);
        if(tag_type < 0) return -ENOTSUP;

        rc = obmafs3_media_tag_put(g_ctx, cat_entry.inode_id, (uint16_t)tag_type, value, (uint32_t)size);
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
        return 0;
    }

    /* user.metadata.<key> */
    const char *meta_key = name + METADATA_XATTR_PREFIX_LEN;
    if(*meta_key == '\0') return -ENOTSUP;
    if(strlen(meta_key) >= METADATA_KEY_MAX) FUSE_RETURN(-ENAMETOOLONG, "");
    if(size >= METADATA_VALUE_MAX) FUSE_RETURN(-ERANGE, "");

    /* Ensure NUL-terminated value */
    char meta_value[METADATA_VALUE_MAX];
    memcpy(meta_value, value, size);
    meta_value[size] = '\0';

    rc = obmafs3_metadata_put(g_ctx, cat_entry.inode_id, meta_key, meta_value);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
    return 0;
}

/**
 * FUSE callback: list extended attribute names.
 *
 * Enumerates all media tag and metadata xattr names attached to a
 * media image file.  Returns the required buffer size when
 * @p size is zero.
 */
static int obmafs3_fuse_listxattr_impl(const char *path, char *list, size_t size)
{
    if(strcmp(path, "/") == 0)
    {
        /* Root directory exposes only the filesystem identity xattr */
        size_t needed = strlen(FSTYPE_XATTR_NAME) + 1; /* include NUL */
        if(size == 0) return (int)needed;
        if(size < needed) FUSE_RETURN(-ERANGE, "");
        memcpy(list, FSTYPE_XATTR_NAME, needed);
        return (int)needed;
    }

    uint64_t    parent_id;
    const char *fname;
    int         rc = resolve_path(path, &parent_id, &fname);
    if(rc != 0) return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, fname, &cat_entry);
    if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENOENT, "");
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    if(!is_image_file_type(inode.file_type)) return 0;

    /* ---- media tag xattrs ---- */
    uint16_t *tag_types = NULL;
    uint32_t  tag_count = 0;
    rc                  = obmafs3_media_tag_list(g_ctx, cat_entry.inode_id, &tag_types, &tag_count);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    size_t total = 0;
    for(uint32_t i = 0; i < tag_count; i++)
    {
        if(tag_types[i] < MEDIA_TAG_XATTR_COUNT && media_tag_xattr_names[tag_types[i]])
        {
            total += MEDIATAG_XATTR_PREFIX_LEN + strlen(media_tag_xattr_names[tag_types[i]]) + 1;
        }
    }

    /* ---- metadata xattrs ---- */
    char   **meta_keys  = NULL;
    uint32_t meta_count = 0;
    rc                  = obmafs3_metadata_list(g_ctx, cat_entry.inode_id, &meta_keys, &meta_count);
    if(rc != OBMAFS3_OK)
    {
        obmafs3_media_tag_list_free(tag_types);
        FUSE_RETURN(-EIO, "");
    }

    for(uint32_t i = 0; i < meta_count; i++) total += METADATA_XATTR_PREFIX_LEN + strlen(meta_keys[i]) + 1;

    if(size == 0)
    {
        obmafs3_media_tag_list_free(tag_types);
        obmafs3_metadata_list_free(meta_keys, meta_count);
        return (int)total;
    }

    if(size < total)
    {
        obmafs3_media_tag_list_free(tag_types);
        obmafs3_metadata_list_free(meta_keys, meta_count);
        FUSE_RETURN(-ERANGE, "");
    }

    char *p = list;
    for(uint32_t i = 0; i < tag_count; i++)
    {
        if(tag_types[i] < MEDIA_TAG_XATTR_COUNT && media_tag_xattr_names[tag_types[i]])
        {
            int n = snprintf(p, size - (size_t)(p - list), "%s%s", MEDIATAG_XATTR_PREFIX,
                             media_tag_xattr_names[tag_types[i]]);
            p += n + 1;
        }
    }
    for(uint32_t i = 0; i < meta_count; i++)
    {
        int n = snprintf(p, size - (size_t)(p - list), "%s%s", METADATA_XATTR_PREFIX, meta_keys[i]);
        p += n + 1;
    }

    obmafs3_media_tag_list_free(tag_types);
    obmafs3_metadata_list_free(meta_keys, meta_count);
    return (int)total;
}

int obmafs3_fuse_listxattr(const char *path, char *list, size_t size)
{
    pthread_rwlock_rdlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_listxattr_impl(path, list, size);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}

/**
 * FUSE callback: remove an extended attribute.
 *
 * Deletes a media tag or metadata entry.  Returns @c -ENODATA if the
 * attribute does not exist.
 */
static int obmafs3_fuse_removexattr_impl(const char *path, const char *name)
{
    int want_mediatag = is_mediatag_xattr(name);
    int want_metadata = is_metadata_xattr(name);

    if(!want_mediatag && !want_metadata) return -ENOTSUP;

    if(strcmp(path, "/") == 0) return -ENOTSUP;

    uint64_t    parent_id;
    const char *fname;
    int         rc = resolve_path(path, &parent_id, &fname);
    if(rc != 0) return rc;

    struct catalog_record cat_entry;
    rc = obmafs3_catalog_lookup(g_ctx, parent_id, fname, &cat_entry);
    if(rc == OBMAFS3_ERR_NOTFOUND) FUSE_RETURN(-ENOENT, "");
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    struct inode_record inode;
    rc = obmafs3_inode_get(g_ctx, cat_entry.inode_id, &inode);
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");

    if(!is_image_file_type(inode.file_type)) return -ENOTSUP;

    if(want_mediatag)
    {
        int tag_type = parse_mediatag_xattr(name);
        if(tag_type < 0) return -ENOTSUP;

        rc = obmafs3_media_tag_delete(g_ctx, cat_entry.inode_id, (uint16_t)tag_type);
        if(rc == OBMAFS3_ERR_NOTFOUND) return -ENODATA;
        if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
        return 0;
    }

    /* user.metadata.<key> */
    const char *meta_key = name + METADATA_XATTR_PREFIX_LEN;
    if(*meta_key == '\0') return -ENOTSUP;

    rc = obmafs3_metadata_delete(g_ctx, cat_entry.inode_id, meta_key);
    if(rc == OBMAFS3_ERR_NOTFOUND) return -ENODATA;
    if(rc != OBMAFS3_OK) FUSE_RETURN(-EIO, "");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Thread-safe wrappers — serialise write-side callbacks              */
/* ------------------------------------------------------------------ */

int obmafs3_fuse_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    pthread_rwlock_wrlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_setxattr_impl(path, name, value, size, flags);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}

int obmafs3_fuse_removexattr(const char *path, const char *name)
{
    pthread_rwlock_wrlock(&g_ctx->tree_lock);
    int rc = obmafs3_fuse_removexattr_impl(path, name);
    pthread_rwlock_unlock(&g_ctx->tree_lock);
    return rc;
}
