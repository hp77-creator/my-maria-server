#include "frm_parser.h"
#include "create_options.h"

bool parse_frm_binary_standalone(THD *thd,
    const uchar *frm_image,
    size_t frm_length,
    TABLE_SHARE *share,
    bool validate_only = false)
{
  uint new_frm_ver, field_pack_length, new_field_pack_flag;
  uint interval_count, interval_parts, read_length, int_length;
  uint total_typelib_value_count;
  uint db_create_options, keys, key_parts, n_length;
  uint com_length, null_bit_pos, UNINIT_VAR(mysql57_vcol_null_bit_pos), bitmap_count;
  uint i, hash_fields= 0;
  bool use_hash, mysql57_null_bits= 0;
  LEX_STRING keynames= {NULL, 0};
  char *names, *comment_pos;
  const uchar *forminfo;
  const uchar *frm_image_end = frm_image + frm_length;
  uchar *record, *null_flags, *null_pos, *UNINIT_VAR(mysql57_vcol_null_pos);
  const uchar *disk_buff, *strpos;
  ulong pos, record_offset;
  ulong rec_buff_length;
  handler *handler_file= 0;
  KEY	*keyinfo;
  KEY_PART_INFO *key_part= NULL;
  Field  **field_ptr, *reg_field;
  const char **interval_array;
  uint *typelib_value_lengths= NULL;
  enum legacy_db_type legacy_db_type;
  my_bitmap_map *bitmaps;
  bool null_bits_are_used;
  uint vcol_screen_length;
  uchar *vcol_screen_pos;
  LEX_CUSTRING options;
  LEX_CSTRING se_name= empty_clex_str;
  KEY first_keyinfo;
  uint len;
  uint ext_key_parts= 0;
  plugin_ref se_plugin= 0;
  bool vers_can_native= false, frm_created= 0;
  Field_data_type_info_array field_data_type_info_array;
  MEM_ROOT *old_root= thd->mem_root;
  Virtual_column_info **table_check_constraints;
  bool *interval_unescaped= NULL;
  extra2_fields extra2;
  bool extra_index_flags_present= FALSE;
  key_map sort_keys_in_use(0);
  DBUG_ENTER("TABLE_SHARE::init_from_binary_frm_image");

  keyinfo= &first_keyinfo;
  thd->mem_root= &share->mem_root;

  if (frm_length < FRM_HEADER_SIZE + FRM_FORMINFO_SIZE)
    goto err;


  share->frm_version= frm_image[2];
  if (share->frm_version < FRM_VER_TRUE_VARCHAR)
    share->keep_original_mysql_version= 1;

  /*
    Check if .frm file created by MySQL 5.0. In this case we want to
    display CHAR fields as CHAR and not as VARCHAR.
    We do it this way as we want to keep the old frm version to enable
    MySQL 4.1 to read these files.
  */
  if (share->frm_version == FRM_VER_TRUE_VARCHAR -1 && frm_image[33] == 5)
    share->frm_version= FRM_VER_TRUE_VARCHAR;

  new_field_pack_flag= frm_image[27];
  new_frm_ver= (frm_image[2] - FRM_VER);
  field_pack_length= new_frm_ver < 2 ? 11 : 17;

  /* Length of the MariaDB extra2 segment in the form file. */
  len = uint2korr(frm_image+4);

  if (read_extra2(frm_image, len, &extra2))
    goto err;

  share->tabledef_version.length= extra2.version.length;
  share->tabledef_version.str= (uchar*)memdup_root(&share->mem_root, extra2.version.str,
                                                       extra2.version.length);
  if (!share->tabledef_version.str)
    goto err;

  /* remember but delay parsing until we have read fields and keys */
  options= extra2.options;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (extra2.engine.str)
  {
    share->default_part_plugin= ha_resolve_by_name(NULL, &extra2.engine, false);
    if (!share->default_part_plugin)
      goto err;
  }
#endif

  if (frm_length < FRM_HEADER_SIZE + len ||
      !(pos= uint4korr(frm_image + FRM_HEADER_SIZE + len)))
    goto err;

  forminfo= frm_image + pos;
  if (forminfo + FRM_FORMINFO_SIZE >= frm_image_end)
    goto err;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (frm_image[61] && !share->default_part_plugin)
  {
    enum legacy_db_type db_type= (enum legacy_db_type) (uint) frm_image[61];
    share->default_part_plugin= ha_lock_engine(NULL, ha_checktype(thd, db_type, 1));
    if (!share->default_part_plugin)
      goto err;
  }
#endif
  legacy_db_type= (enum legacy_db_type) (uint) frm_image[3];
  /*
    if the storage engine is dynamic, no point in resolving it by its
    dynamically allocated legacy_db_type. We will resolve it later by name.
  */
  if (legacy_db_type > DB_TYPE_UNKNOWN &&
      legacy_db_type < DB_TYPE_FIRST_DYNAMIC)
    se_plugin= ha_lock_engine(NULL, ha_checktype(thd, legacy_db_type));
  share->db_create_options= db_create_options= uint2korr(frm_image+30);
  share->db_options_in_use= share->db_create_options;
  share->mysql_version= uint4korr(frm_image+51);
  share->table_type= TABLE_TYPE_NORMAL;
  share->null_field_first= 0;
  if (!frm_image[32])				// New frm file in 3.23
  {
    uint cs_org= (((uint) frm_image[41]) << 8) + (uint) frm_image[38];
    uint cs_new= Charset::upgrade_collation_id(share->mysql_version, cs_org);
    if (cs_org != cs_new)
      share->incompatible_version|= HA_CREATE_USED_CHARSET;

    share->avg_row_length= uint4korr(frm_image+34);
    share->transactional= (ha_choice)
      enum_value_with_check(thd, share, "transactional", frm_image[39] & 3, HA_CHOICE_MAX);
    share->page_checksum= (ha_choice)
      enum_value_with_check(thd, share, "page_checksum", (frm_image[39] >> 2) & 3, HA_CHOICE_MAX);
    if (((ha_choice) enum_value_with_check(thd, share, "sequence",
                                           (frm_image[39] >> 4) & 3,
                                           HA_CHOICE_MAX)) == HA_CHOICE_YES)
    {
      share->table_type= TABLE_TYPE_SEQUENCE;
      share->sequence= new (&share->mem_root) SEQUENCE();
      share->non_determinstic_insert= true;
    }
    share->row_type= (enum row_type)
      enum_value_with_check(thd, share, "row_format", frm_image[40], ROW_TYPE_MAX);

    if (cs_new && !(share->table_charset= get_charset(cs_new, MYF(MY_WME))))
      goto err;
    share->null_field_first= 1;
    share->stats_sample_pages= uint2korr(frm_image+42);
    share->stats_auto_recalc= (enum_stats_auto_recalc)(frm_image[44]);
    share->table_check_constraints= uint2korr(frm_image+45);
  }
  if (!share->table_charset)
  {
    const CHARSET_INFO *cs= thd->variables.collation_database;
    /* unknown charset in frm_image[38] or pre-3.23 frm */
    if (cs->use_mb())
    {
      /* Warn that we may be changing the size of character columns */
      sql_print_warning("'%s' had no or invalid character set, "
                        "and default character set is multi-byte, "
                        "so character column sizes may have changed",
                        share->path.str);
    }
    share->table_charset= cs;
  }

  share->db_record_offset= 1;
  share->max_rows= uint4korr(frm_image+18);
  share->min_rows= uint4korr(frm_image+22);

  /* Read keyinformation */
  disk_buff= frm_image + uint2korr(frm_image+6);

  if (disk_buff + 6 >= frm_image_end)
    goto err;

  if (disk_buff[0] & 0x80)
  {
    keys=      (disk_buff[1] << 7) | (disk_buff[0] & 0x7f);
    share->key_parts= key_parts= uint2korr(disk_buff+2);
  }
  else
  {
    keys=      disk_buff[0];
    share->key_parts= key_parts= disk_buff[1];
  }
  share->keys_for_keyread.init(0);
  share->ignored_indexes.init(0);
  share->keys_in_use.init(keys);
  ext_key_parts= key_parts;

  if (extra2.index_flags.str && extra2.index_flags.length != keys)
    goto err;

  len= (uint) uint2korr(disk_buff+4);

  share->reclength = uint2korr(frm_image+16);
  share->stored_rec_length= share->reclength;
  if (frm_image[26] == 1)
    share->system= 1;				/* one-record-database */

  record_offset= (ulong) (uint2korr(frm_image+6)+
                          ((uint2korr(frm_image+14) == 0xffff ?
                            uint4korr(frm_image+47) : uint2korr(frm_image+14))));

  if (record_offset + share->reclength >= frm_length)
    goto err;

  if ((n_length= uint4korr(frm_image+55)))
  {
    /* Read extra data segment */
    const uchar *next_chunk, *buff_end;
    DBUG_PRINT("info", ("extra segment size is %u bytes", n_length));
    next_chunk= frm_image + record_offset + share->reclength;
    buff_end= next_chunk + n_length;

    if (buff_end >= frm_image_end)
      goto err;

    share->connect_string.length= uint2korr(next_chunk);
    if (!(share->connect_string.str= strmake_root(&share->mem_root,
                                                  (char*) next_chunk + 2,
                                                  share->connect_string.
                                                  length)))
    {
      goto err;
    }
    next_chunk+= share->connect_string.length + 2;
    if (next_chunk + 2 < buff_end)
    {
      uint str_db_type_length= uint2korr(next_chunk);
      se_name.str= (char*) next_chunk + 2;
      se_name.length= str_db_type_length;

      plugin_ref tmp_plugin= ha_resolve_by_name(thd, &se_name, false);
      if (tmp_plugin != NULL && !plugin_equals(tmp_plugin, se_plugin) &&
          legacy_db_type != DB_TYPE_S3)
      {
        if (se_plugin)
        {
          /* bad file, legacy_db_type did not match the name */
          sql_print_warning("%s.frm is inconsistent: engine typecode %d, engine name %s (%d)",
                        share->normalized_path.str, legacy_db_type,
                        plugin_name(tmp_plugin)->str,
                        ha_legacy_type(plugin_data(tmp_plugin, handlerton *)));
        }
        /*
          tmp_plugin is locked with a local lock.
          we unlock the old value of se_plugin before
          replacing it with a globally locked version of tmp_plugin
        */
        plugin_unlock(NULL, se_plugin);
        se_plugin= plugin_lock(NULL, tmp_plugin);
      }
#ifdef WITH_PARTITION_STORAGE_ENGINE
      else if (str_db_type_length == 9 &&
               !strncmp((char *) next_chunk + 2, "partition", 9))
      {
        if (change_to_partiton_engine(&se_plugin))
          goto err;
      }
#endif
      else if (!tmp_plugin)
      {
        /* purecov: begin inspected */
        ((char*) se_name.str)[se_name.length]=0;
        my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), se_name.str);
        goto err;
        /* purecov: end */
      }
      next_chunk+= str_db_type_length + 2;
    }

    /*
      Check if engine supports extended keys. This is used by
      create_key_infos() to allocate room for extended keys
    */
    share->set_use_ext_keys_flag(plugin_hton(se_plugin)->flags &
                                 HTON_SUPPORTS_EXTENDED_KEYS);

    if (create_key_infos(disk_buff + 6, frm_image_end, keys, keyinfo,
                         new_frm_ver, &ext_key_parts,
                         share, len, &first_keyinfo, &keynames))
      goto err;

    if (next_chunk + 5 < buff_end)
    {
      uint32 partition_info_str_len = uint4korr(next_chunk);
#ifdef WITH_PARTITION_STORAGE_ENGINE
      if ((share->partition_info_buffer_size=
             share->partition_info_str_len= partition_info_str_len))
      {
        if (!(share->partition_info_str= (char*)
              memdup_root(&share->mem_root, next_chunk + 4,
                          partition_info_str_len + 1)))
        {
          goto err;
        }
        if (plugin_data(se_plugin, handlerton*) != partition_hton &&
            share->mysql_version >= 50600 && share->mysql_version <= 50799)
        {
          share->keep_original_mysql_version= 1;
          if (change_to_partiton_engine(&se_plugin))
            goto err;
        }
      }
#else
      if (partition_info_str_len)
      {
        DBUG_PRINT("info", ("WITH_PARTITION_STORAGE_ENGINE is not defined"));
        goto err;
      }
#endif
      next_chunk+= 5 + partition_info_str_len;
    }
    if (share->mysql_version >= 50110 && next_chunk < buff_end)
    {
      /* New auto_partitioned indicator introduced in 5.1.11 */
#ifdef WITH_PARTITION_STORAGE_ENGINE
      share->auto_partitioned= *next_chunk;
#endif
      next_chunk++;
    }
    keyinfo= share->key_info;
    for (i= 0; i < keys; i++, keyinfo++)
    {
      if (keyinfo->flags & HA_USES_PARSER)
      {
        LEX_CSTRING parser_name;
        if (next_chunk >= buff_end)
        {
          DBUG_PRINT("error",
                     ("fulltext key uses parser that is not defined in .frm"));
          goto err;
        }
        parser_name.str= (char*) next_chunk;
        parser_name.length= strlen((char*) next_chunk);
        next_chunk+= parser_name.length + 1;
        keyinfo->parser= my_plugin_lock_by_name(NULL, &parser_name,
                                                MYSQL_FTPARSER_PLUGIN);
        if (! keyinfo->parser)
        {
          my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), parser_name.str);
          goto err;
        }
      }
    }

    if (forminfo[46] == (uchar)255)
    {
      //reading long table comment
      if (next_chunk + 2 > buff_end)
      {
          DBUG_PRINT("error",
                     ("long table comment is not defined in .frm"));
          goto err;
      }
      share->comment.length = uint2korr(next_chunk);
      if (! (share->comment.str= strmake_root(&share->mem_root,
             (char*)next_chunk + 2, share->comment.length)))
      {
          goto err;
      }
      next_chunk+= 2 + share->comment.length;
    }

    DBUG_ASSERT(next_chunk <= buff_end);

    if (share->db_create_options & HA_OPTION_TEXT_CREATE_OPTIONS_legacy)
    {
      if (options.str)
        goto err;
      options.length= uint4korr(next_chunk);
      options.str= next_chunk + 4;
      next_chunk+= options.length + 4;
    }
    DBUG_ASSERT(next_chunk <= buff_end);
  }
  else
  {
    if (create_key_infos(disk_buff + 6, frm_image_end, keys, keyinfo,
                         new_frm_ver, &ext_key_parts,
                         share, len, &first_keyinfo, &keynames))
      goto err;
  }
  share->key_block_size= uint2korr(frm_image+62);
  keyinfo= share->key_info;


  if (extra2.index_flags.str)
    extra_index_flags_present= TRUE;

  for (uint i= 0; i < share->total_keys; i++, keyinfo++)
  {
    if (extra_index_flags_present)
    {
      uchar flags= *extra2.index_flags.str++;
      keyinfo->is_ignored= (flags & EXTRA2_IGNORED_KEY);
    }
    else
      keyinfo->is_ignored= FALSE;

    if (keyinfo->algorithm == HA_KEY_ALG_LONG_HASH)
      hash_fields++;
  }

  share->set_ignored_indexes();

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (par_image && plugin_data(se_plugin, handlerton*) == partition_hton)
  {
    /*
      Discovery returned a partition plugin. Change to use it. The partition
      engine will then use discovery to find the rest of the plugin tables,
      which may be in the original engine used for discovery
    */
    share->db_plugin= se_plugin;
  }
#endif
  if (share->db_plugin && !plugin_equals(share->db_plugin, se_plugin))
    goto err; // wrong engine (someone changed the frm under our feet?)

  rec_buff_length= ALIGN_SIZE(share->reclength + 1);
  share->rec_buff_length= rec_buff_length;
  if (!(record= (uchar *) alloc_root(&share->mem_root, rec_buff_length)))
    goto err;                          /* purecov: inspected */
  /* Mark bytes after record as not accessable to catch overrun bugs */
  MEM_NOACCESS(record + share->reclength, rec_buff_length - share->reclength);
  share->default_values= record;
  memcpy(record, frm_image + record_offset, share->reclength);

  disk_buff= frm_image + pos + FRM_FORMINFO_SIZE;
  share->fields= uint2korr(forminfo+258);
  if (extra2.field_flags.str && extra2.field_flags.length != share->fields)
    goto err;
  pos= uint2korr(forminfo+260);   /* Length of all screens */
  n_length= uint2korr(forminfo+268);
  interval_count= uint2korr(forminfo+270);
  interval_parts= uint2korr(forminfo+272);
  int_length= uint2korr(forminfo+274);
  share->null_fields= uint2korr(forminfo+282);
  com_length= uint2korr(forminfo+284);
  vcol_screen_length= uint2korr(forminfo+286);
  share->virtual_fields= share->default_expressions=
    share->field_check_constraints= share->default_fields= 0;
  share->visible_fields= 0;
  share->stored_fields= share->fields;
  if (forminfo[46] != (uchar)255)
  {
    share->comment.length=  (int) (forminfo[46]);
    share->comment.str= strmake_root(&share->mem_root, (char*) forminfo+47,
                                     share->comment.length);
  }

  DBUG_PRINT("info",("i_count: %d  i_parts: %d  index: %d  n_length: %d  int_length: %d  com_length: %d  vcol_screen_length: %d", interval_count,interval_parts, keys,n_length,int_length, com_length, vcol_screen_length));

  /*
    We load the following things into TYPELIBs:
    - One TYPELIB for field names
    - interval_count TYPELIBs for ENUM/SET values
    - One TYPELIB for key names
    Every TYPELIB requires one extra value with a NULL pointer and zero length,
    which is the end-of-values marker.
    TODO-10.5+:
    Note, we should eventually reuse this total_typelib_value_count
    to allocate interval_array. The below code reserves less space
    than total_typelib_value_count pointers. So it seems `interval_array`
    and `names` overlap in the memory. Too dangerous to fix in 10.1.
  */
  total_typelib_value_count=
    (share->fields  +              1/*end-of-values marker*/) +
    (interval_parts + interval_count/*end-of-values markers*/) +
    (keys           +              1/*end-of-values marker*/);

  if (!multi_alloc_root(&share->mem_root,
                        &share->field, (uint)(share->fields+1)*sizeof(Field*),
                        &share->intervals, (uint)interval_count*sizeof(TYPELIB),
                        &share->check_constraints, (uint) share->table_check_constraints * sizeof(Virtual_column_info*),
                        /*
                           This looks wrong: shouldn't it be (+2+interval_count)
                           instread of (+3) ?
                        */
                        &interval_array, (uint) (share->fields+interval_parts+ keys+3)*sizeof(char *),
                        &typelib_value_lengths, total_typelib_value_count * sizeof(uint *),
                        &names, (uint) (n_length+int_length),
                        &comment_pos, (uint) com_length,
                        &vcol_screen_pos, vcol_screen_length,
                        NullS))

    goto err;

  if (interval_count)
  {
    if (!(interval_unescaped= (bool*) my_alloca(interval_count * sizeof(bool))))
      goto err;
    bzero(interval_unescaped, interval_count * sizeof(bool));
  }

  field_ptr= share->field;
  table_check_constraints= share->check_constraints;
  read_length=(uint) (share->fields * field_pack_length +
		      pos+ (uint) (n_length+int_length+com_length+
		                   vcol_screen_length));
  strpos= disk_buff+pos;

  if (!interval_count)
    share->intervals= 0;			// For better debugging

  share->vcol_defs.str= vcol_screen_pos;
  share->vcol_defs.length= vcol_screen_length;

  memcpy(names, strpos+(share->fields*field_pack_length), n_length+int_length);
  memcpy(comment_pos, disk_buff+read_length-com_length-vcol_screen_length,
         com_length);
  memcpy(vcol_screen_pos, disk_buff+read_length-vcol_screen_length,
         vcol_screen_length);

  if (fix_type_pointers(&interval_array, &typelib_value_lengths,
                        &share->fieldnames, 1, names, n_length) ||
      share->fieldnames.count != share->fields)
    goto err;

  if (fix_type_pointers(&interval_array, &typelib_value_lengths,
                        share->intervals, interval_count,
                        names + n_length, int_length))
    goto err;

  if (keynames.length &&
      (fix_type_pointers(&interval_array, &typelib_value_lengths,
                         &share->keynames, 1, keynames.str, keynames.length) ||
      share->keynames.count != keys))
    goto err;

  /* Allocate handler */
  if (!(handler_file= get_new_handler(share, thd->mem_root,
                                      plugin_hton(se_plugin))))
    goto err;

  if (handler_file->set_ha_share_ref(&share->ha_share))
    goto err;

  record= share->default_values-1;              /* Fieldstart = 1 */
  null_bits_are_used= share->null_fields != 0;
  if (share->null_field_first)
  {
    null_flags= null_pos= record+1;
    null_bit_pos= (db_create_options & HA_OPTION_PACK_RECORD) ? 0 : 1;
    /*
      null_bytes below is only correct under the condition that
      there are no bit fields.  Correct values is set below after the
      table struct is initialized
    */
    share->null_bytes= (share->null_fields + null_bit_pos + 7) / 8;
  }
#ifndef WE_WANT_TO_SUPPORT_VERY_OLD_FRM_FILES
  else
  {
    share->null_bytes= (share->null_fields+7)/8;
    null_flags= null_pos= record + 1 + share->reclength - share->null_bytes;
    null_bit_pos= 0;
  }
#endif

  use_hash= share->fields >= MAX_FIELDS_BEFORE_HASH;
  if (use_hash)
    use_hash= !my_hash_init(PSI_INSTRUMENT_ME, &share->name_hash,
                            Lex_ident_column::charset_info(),
                            share->fields, 0, 0, get_field_name, 0, 0);

  if (share->mysql_version >= 50700 && share->mysql_version < 100000 &&
      vcol_screen_length)
  {
    share->keep_original_mysql_version= 1;
    /*
      MySQL 5.7 stores the null bits for not stored fields last.
      Calculate the position for them.
    */
    mysql57_null_bits= 1;
    mysql57_vcol_null_pos= null_pos;
    mysql57_vcol_null_bit_pos= null_bit_pos;
    mysql57_calculate_null_position(share, &mysql57_vcol_null_pos,
                                    &mysql57_vcol_null_bit_pos,
                                    strpos, vcol_screen_pos);
  }

  /* Set system versioning information. */
  vers.name= "SYSTEM_TIME"_Lex_ident_column;
  if (extra2.system_period.str == NULL)
  {
    versioned= VERS_UNDEFINED;
    vers.start_fieldno= 0;
    vers.end_fieldno= 0;
  }
  else
  {
    DBUG_PRINT("info", ("Setting system versioning information"));
    if (init_period_from_extra2(&vers, extra2.system_period.str,
                  extra2.system_period.str + extra2.system_period.length))
      goto err;
    DBUG_PRINT("info", ("Columns with system versioning: [%d, %d]",
                        vers.start_fieldno, vers.end_fieldno));
    versioned= VERS_TIMESTAMP;
    vers_can_native= handler_file->vers_can_native(thd);
    status_var_increment(thd->status_var.feature_system_versioning);
  } // if (system_period == NULL)

  if (extra2.application_period.str)
  {
    const uchar *pos= extra2.application_period.str;
    const uchar *end= pos + extra2.application_period.length;
    period.name.length= extra2_read_len(&pos, end);
    period.name.str= strmake_root(&mem_root, (char*)pos, period.name.length);
    pos+= period.name.length;

    period.constr_name.length= extra2_read_len(&pos, end);
    period.constr_name.str= strmake_root(&mem_root, (char*)pos,
                                         period.constr_name.length);
    pos+= period.constr_name.length;

    if (init_period_from_extra2(&period, pos, end))
      goto err;
    if (extra2_str_size(period.name.length)
         + extra2_str_size(period.constr_name.length)
         + 2 * frm_fieldno_size
        != extra2.application_period.length)
      goto err;
    status_var_increment(thd->status_var.feature_application_time_periods);
  }

  if (extra2.without_overlaps.str)
  {
    if (extra2.application_period.str == NULL)
      goto err;
    const uchar *key_pos= extra2.without_overlaps.str;
    period.unique_keys= read_frm_keyno(key_pos);
    for (uint k= 0; k < period.unique_keys; k++)
    {
      key_pos+= frm_keyno_size;
      uint key_nr= read_frm_keyno(key_pos);
      key_info[key_nr].without_overlaps= true;
    }

    if ((period.unique_keys + 1) * frm_keyno_size
        != extra2.without_overlaps.length)
      goto err;
  }

  if (extra2.field_data_type_info.length &&
      field_data_type_info_array.parse(old_root, share->fields,
                                       extra2.field_data_type_info))
    goto err;

  /*
    Column definitions extraction begins here.
  */
  for (i=0 ; i < share->fields; i++, strpos+=field_pack_length, field_ptr++)
  {
    uint interval_nr= 0, recpos;
    LEX_CSTRING comment;
    LEX_CSTRING name;
    Virtual_column_info *vcol_info= 0;
    const Type_handler *handler;
    uint32 flags= 0;
    Column_definition_attributes attr;

    if (new_frm_ver >= 3)
    {
      /* new frm file in 4.1 */
      recpos=	    uint3korr(strpos+5);
      uint comment_length=uint2korr(strpos+15);

      if (!comment_length)
      {
	comment.str= (char*) "";
	comment.length=0;
      }
      else
      {
	comment.str=    (char*) comment_pos;
	comment.length= comment_length;
	comment_pos+=   comment_length;
      }

      if (strpos[13] == MYSQL_TYPE_VIRTUAL &&
          (share->mysql_version < 50600 || share->mysql_version >= 100000))
      {
        /*
          MariaDB 5.5 or 10.0 version.
          The interval_id byte in the .frm file stores the length of the
          expression statement for a virtual column.
        */
        uint vcol_info_length= (uint) strpos[12];

        if (!vcol_info_length) // Expect non-null expression
          goto err;

        attr.frm_unpack_basic(strpos);
        if (attr.frm_unpack_charset(share, strpos))
          goto err;
        /*
          Old virtual field information before 10.2

          Get virtual column data stored in the .frm file as follows:
          byte 1      = 1 | 2
          byte 2      = sql_type
          byte 3      = flags. 1 for stored_in_db
          [byte 4]    = optional interval_id for sql_type (if byte 1 == 2)
          next byte ...  = virtual column expression (text data)
        */

        vcol_info= new (&share->mem_root) Virtual_column_info();
        bool opt_interval_id= (uint)vcol_screen_pos[0] == 2;
        enum_field_types ftype= (enum_field_types) (uchar) vcol_screen_pos[1];
        if (!(handler= Type_handler::get_handler_by_real_type(ftype)))
          goto err;
        if (opt_interval_id)
          interval_nr= (uint)vcol_screen_pos[3];
        else if ((uint)vcol_screen_pos[0] != 1)
          goto err;
        bool stored= vcol_screen_pos[2] & 1;
        vcol_info->set_vcol_type(stored ? VCOL_GENERATED_STORED : VCOL_GENERATED_VIRTUAL);
        uint vcol_expr_length= vcol_info_length -
                              (uint)(FRM_VCOL_OLD_HEADER_SIZE(opt_interval_id));
        vcol_info->utf8= 0; // before 10.2.1 the charset was unknown
        int2store(vcol_screen_pos+1, vcol_expr_length); // for parse_vcol_defs()
        vcol_screen_pos+= vcol_info_length;
        share->virtual_fields++;
      }
      else
      {
        interval_nr=  (uint) strpos[12];
        enum_field_types field_type= (enum_field_types) strpos[13];
        if (!(handler= Type_handler::get_handler_by_real_type(field_type)))
        {
          if (field_type == 245 &&
              share->mysql_version >= 50700) // a.k.a MySQL 5.7 JSON
          {
            share->incompatible_version|= HA_CREATE_USED_ENGINE;
            const LEX_CSTRING mysql_json{STRING_WITH_LEN("MYSQL_JSON")};
            handler= Type_handler::handler_by_name_or_error(thd, mysql_json);
          }

          if (!handler)
            goto err; // Not supported field type
        }
        handler= handler->type_handler_frm_unpack(strpos);
        if (handler->Column_definition_attributes_frm_unpack(&attr, share,
                                                             strpos,
                                                             &extra2.gis))
          goto err;

        if (field_data_type_info_array.count())
        {
          const LEX_CSTRING &info= field_data_type_info_array.
                                     element(i).type_info();
          DBUG_EXECUTE_IF("frm_data_type_info",
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
            ER_UNKNOWN_ERROR, "DBUG: [%u] name='%s' type_info='%.*s'",
            i, share->fieldnames.type_names[i],
            (uint) info.length, info.str););

          if (info.length)
          {
            const Type_handler *h= Type_handler::handler_by_name_or_error(thd,
                                                                          info);
            /*
              This code will eventually be extended here:
              - If the handler was not found by name, we could
                still open the table using the fallback type handler "handler",
                at least for a limited set of commands.
              - If the handler was found by name, we could check
                that "h" and "handler" have the same type code
                (and maybe some other properties) to make sure
                that the FRM data is consistent.
            */
            if (!h)
              goto err;
            handler= h;
          }
        }
      }

      if (((uint) strpos[10]) & MYSQL57_GENERATED_FIELD)
      {
        attr.unireg_check= Field::NONE;

        /*
          MySQL 5.7 generated fields

          byte 1        = 1
          byte 2,3      = expr length
          byte 4        = stored_in_db
          byte 5..      = expr
        */
        if ((uint)(vcol_screen_pos)[0] != 1)
          goto err;
        vcol_info= new (&share->mem_root) Virtual_column_info();
        uint vcol_info_length= uint2korr(vcol_screen_pos + 1);
        if (!vcol_info_length) // Expect non-empty expression
          goto err;
        vcol_info->set_vcol_type(vcol_screen_pos[3] ? VCOL_GENERATED_STORED : VCOL_GENERATED_VIRTUAL);
        vcol_info->utf8= 0;
        vcol_screen_pos+= vcol_info_length + MYSQL57_GCOL_HEADER_SIZE;;
        share->virtual_fields++;
      }
    }
    else
    {
      attr.length= (uint) strpos[3];
      recpos=	    uint2korr(strpos+4),
      attr.pack_flag=    uint2korr(strpos+6);
      if (f_is_num(attr.pack_flag))
      {
        attr.decimals= f_decimals(attr.pack_flag);
        attr.pack_flag&= ~FIELDFLAG_DEC_MASK;
      }
      attr.pack_flag&=   ~FIELDFLAG_NO_DEFAULT;     // Safety for old files
      attr.unireg_check=  (Field::utype) MTYP_TYPENR((uint) strpos[8]);
      interval_nr=  (uint) strpos[10];

      /* old frm file */
      enum_field_types ftype= (enum_field_types) f_packtype(attr.pack_flag);
      if (!(handler= Type_handler::get_handler_by_real_type(ftype)))
        goto err; // Not supported field type

      if (f_is_binary(attr.pack_flag))
      {
        /*
          Try to choose the best 4.1 type:
          - for 4.0 "CHAR(N) BINARY" or "VARCHAR(N) BINARY"
           try to find a binary collation for character set.
          - for other types (e.g. BLOB) just use my_charset_bin.
        */
        if (!f_is_blob(attr.pack_flag))
        {
          // 3.23 or 4.0 string
          myf utf8_flag= thd->get_utf8_flag();
          if (!(attr.charset= get_charset_by_csname(share->table_charset->
                                                    cs_name.str,
                                                    MY_CS_BINSORT,
                                                    MYF(utf8_flag))))
            attr.charset= &my_charset_bin;
        }
      }
      else
        attr.charset= share->table_charset;
      bzero((char*) &comment, sizeof(comment));
      if ((!(handler= old_frm_type_handler(attr.pack_flag, interval_nr))))
        goto err; // Not supported field type
    }

    /* Remove >32 decimals from old files */
    if (share->mysql_version < 100200 &&
        (attr.pack_flag & FIELDFLAG_LONG_DECIMAL))
    {
      share->keep_original_mysql_version= 1;
      attr.pack_flag&= ~FIELDFLAG_LONG_DECIMAL;
    }

    if (interval_nr && attr.charset->mbminlen > 1 &&
        !interval_unescaped[interval_nr - 1])
    {
      /*
        Unescape UCS2/UTF16/UTF32 intervals from HEX notation.
        Note, ENUM/SET columns with equal value list share a single
        copy of TYPELIB. Unescape every TYPELIB only once.
      */
      TYPELIB *interval= share->intervals + interval_nr - 1;
      unhex_type2(interval);
      interval_unescaped[interval_nr - 1]= true;
    }

#ifndef TO_BE_DELETED_ON_PRODUCTION
    if (handler->real_field_type() == MYSQL_TYPE_NEWDECIMAL &&
        !share->mysql_version)
    {
      /*
        Fix pack length of old decimal values from 5.0.3 -> 5.0.4
        The difference is that in the old version we stored precision
        in the .frm table while we now store the display_length
      */
      uint decimals= f_decimals(attr.pack_flag);
      attr.length=
        my_decimal_precision_to_length((uint) attr.length, decimals,
                                       f_is_dec(attr.pack_flag) == 0);
      sql_print_error("Found incompatible DECIMAL field '%s' in %s; "
                      "Please do \"ALTER TABLE '%s' FORCE\" to fix it!",
                      share->fieldnames.type_names[i], share->table_name.str,
                      share->table_name.str);
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_CRASHED_ON_USAGE,
                          "Found incompatible DECIMAL field '%s' in %s; "
                          "Please do \"ALTER TABLE '%s' FORCE\" to fix it!",
                          share->fieldnames.type_names[i],
                          share->table_name.str,
                          share->table_name.str);
      share->crashed= 1;                        // Marker for CHECK TABLE
    }
#endif

    if (mysql57_null_bits && vcol_info && !vcol_info->is_stored())
    {
      swap_variables(uchar*, null_pos, mysql57_vcol_null_pos);
      swap_variables(uint, null_bit_pos, mysql57_vcol_null_bit_pos);
    }

    if (versioned)
    {
      if (i == vers.start_fieldno)
        flags|= VERS_ROW_START;
      else if (i == vers.end_fieldno)
        flags|= VERS_ROW_END;

      if (flags & VERS_SYSTEM_FIELD)
      {
        auto field_type= handler->real_field_type();

        DBUG_EXECUTE_IF("error_vers_wrong_type", field_type= MYSQL_TYPE_BLOB;);

        switch (field_type) {
        case MYSQL_TYPE_TIMESTAMP2:
          break;
        case MYSQL_TYPE_LONGLONG:
          if (vers_can_native)
          {
            versioned= VERS_TRX_ID;
            break;
          }
          /* Fallthrough */
        default:
          my_error(ER_VERS_FIELD_WRONG_TYPE,
                   (field_type == MYSQL_TYPE_LONGLONG ?
                    MYF(0) : MYF(ME_WARNING)),
                   fieldnames.type_names[i],
                   (versioned == VERS_TIMESTAMP ?
                    "TIMESTAMP(6)" : "BIGINT(20) UNSIGNED"),
                   table_name.str);
          goto err;
        }
      }
    }

    /* Convert pre-10.2.2 timestamps to use Field::default_value */
    name.str= fieldnames.type_names[i];
    name.length= strlen(name.str);
    attr.set_typelib(interval_nr ? share->intervals + interval_nr - 1 : NULL);
    Record_addr addr(record + recpos, null_pos, null_bit_pos);
    *field_ptr= reg_field=
      attr.make_field(share, &share->mem_root, &addr, handler, &name, flags);
    if (!reg_field)				// Not supported field type
      goto err;

    if (attr.unireg_check == Field::TIMESTAMP_DNUN_FIELD ||
        attr.unireg_check == Field::TIMESTAMP_DN_FIELD)
    {
      reg_field->default_value= new (&share->mem_root) Virtual_column_info();
      reg_field->default_value->set_vcol_type(VCOL_DEFAULT);
      share->default_expressions++;
    }

    reg_field->field_index= i;
    reg_field->comment=comment;
    reg_field->vcol_info= vcol_info;
    reg_field->flags|= flags;
    if (extra2.field_flags.str)
    {
      uchar flags= *extra2.field_flags.str++;
      if (flags & VERS_OPTIMIZED_UPDATE)
        reg_field->flags|= VERS_UPDATE_UNVERSIONED_FLAG;

      reg_field->invisible= f_visibility(flags);
    }
    if (reg_field->invisible == INVISIBLE_USER)
      status_var_increment(thd->status_var.feature_invisible_columns);
    if (!reg_field->invisible)
      share->visible_fields++;
    if (handler->real_field_type() == MYSQL_TYPE_BIT &&
        !f_bit_as_char(attr.pack_flag))
    {
      null_bits_are_used= 1;
      if ((null_bit_pos+= (uint) (attr.length & 7)) > 7)
      {
        null_pos++;
        null_bit_pos-= 8;
      }
    }
    if (!(reg_field->flags & NOT_NULL_FLAG))
    {
      if (!(null_bit_pos= (null_bit_pos + 1) & 7))
        null_pos++;
    }

    if (vcol_info)
    {
      vcol_info->name= reg_field->field_name;
      if (mysql57_null_bits && !vcol_info->is_stored())
      {
        /* MySQL 5.7 has null bits last */
        swap_variables(uchar*, null_pos, mysql57_vcol_null_pos);
        swap_variables(uint, null_bit_pos, mysql57_vcol_null_bit_pos);
      }
    }

    if (f_no_default(attr.pack_flag))
      reg_field->flags|= NO_DEFAULT_VALUE_FLAG;

    if (reg_field->unireg_check == Field::NEXT_NUMBER)
      share->found_next_number_field= field_ptr;

    if (use_hash && my_hash_insert(&share->name_hash, (uchar*) field_ptr))
      goto err;
    if (!reg_field->stored_in_db())
    {
      share->stored_fields--;
      if (share->stored_rec_length>=recpos)
        share->stored_rec_length= recpos-1;
    }
    if (reg_field->has_update_default_function())
    {
      has_update_default_function= 1;
      if (!reg_field->default_value)
        share->default_fields++;
    }
  }
  /*
    Column definitions extraction ends here.
  */

  *field_ptr=0;					// End marker
  /* Sanity checks: */
  DBUG_ASSERT(share->fields>=share->stored_fields);
  DBUG_ASSERT(share->reclength>=share->stored_rec_length);

  if (mysql57_null_bits)
  {
    /* We want to store the value for the last bits */
    swap_variables(uchar*, null_pos, mysql57_vcol_null_pos);
    swap_variables(uint, null_bit_pos, mysql57_vcol_null_bit_pos);
    DBUG_ASSERT((null_pos + (null_bit_pos + 7) / 8) <= share->field[0]->ptr);
  }

  share->primary_key= MAX_KEY;

  /* Fix key->name and key_part->field */
  if (key_parts)
  {
    keyinfo= share->key_info;
    uint hash_field_used_no= share->fields - hash_fields;
    KEY_PART_INFO *hash_keypart;
    Field *hash_field;
    uint offset= share->reclength - HA_HASH_FIELD_LENGTH * hash_fields;
    for (uint i= 0; i < share->keys; i++, keyinfo++)
    {
      /* We need set value in hash key_part */
      if (keyinfo->algorithm == HA_KEY_ALG_LONG_HASH)
      {
        share->long_unique_table= 1;
        hash_keypart= keyinfo->key_part + keyinfo->user_defined_key_parts;
        hash_keypart->length= HA_HASH_KEY_LENGTH_WITHOUT_NULL;
        hash_keypart->store_length= hash_keypart->length;
        hash_keypart->type= HA_KEYTYPE_ULONGLONG;
        hash_keypart->key_part_flag= 0;
        hash_keypart->key_type= 32834;
        /* Last n fields are unique_index_hash fields */
        hash_keypart->offset= offset;
        hash_keypart->fieldnr= hash_field_used_no + 1;
        hash_field= share->field[hash_field_used_no];
        hash_field->flags|= LONG_UNIQUE_HASH_FIELD;//Used in parse_vcol_defs
        keyinfo->flags|= HA_NOSAME;
        share->virtual_fields++;
        share->stored_fields--;
        if (record + share->stored_rec_length >= hash_field->ptr)
          share->stored_rec_length= (ulong)(hash_field->ptr - record - 1);
        hash_field_used_no++;
        offset+= HA_HASH_FIELD_LENGTH;
      }
    }
    longlong ha_option= handler_file->ha_table_flags();
    keyinfo= share->key_info;
    uint primary_key= Lex_ident_column(share->keynames.type_names[0],
                                       share->keynames.type_lengths[0]).
                                         streq(primary_key_name) ? 0 : MAX_KEY;
    KEY* key_first_info= NULL;

    if (primary_key >= MAX_KEY && keyinfo->flags & HA_NOSAME &&
        keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
    {
      /*
        If the UNIQUE key doesn't have NULL columns and is not a part key
        declare this as a primary key.
      */
      primary_key= 0;
      key_part= keyinfo->key_part;
      for (i=0 ; i < keyinfo->user_defined_key_parts ;i++)
      {
        DBUG_ASSERT(key_part[i].fieldnr > 0);
        // Table field corresponding to the i'th key part.
        Field *table_field= share->field[key_part[i].fieldnr - 1];

        /*
          If the key column is of NOT NULL BLOB type, then it
          will definitely have key prefix. And if key part prefix size
          is equal to the BLOB column max size, then we can promote
          it to primary key.
        */
        if (!table_field->real_maybe_null() &&
            table_field->type() == MYSQL_TYPE_BLOB &&
            table_field->field_length == key_part[i].length)
          continue;

        if (table_field->real_maybe_null() ||
            table_field->key_length() != key_part[i].length)
        {
          primary_key= MAX_KEY;		// Can't be used
          break;
        }
      }
    }

    /*
      Make sure that the primary key is not marked as IGNORE
      This can happen in the case
        1) when IGNORE is mentioned in the Key specification
        2) When a unique NON-NULLABLE key is promoted to a primary key.
           The unique key could have been marked as IGNORE when there
           was a primary key in the table.

           Eg:
            CREATE TABLE t1(a INT NOT NULL, primary key(a), UNIQUE key1(a))
             so for this table when we try to IGNORE key1
             then we run:
                ALTER TABLE t1 ALTER INDEX key1 IGNORE
              this runs successfully and key1 is marked as IGNORE.

              But lets say then we drop the primary key
               ALTER TABLE t1 DROP PRIMARY
              then the UNIQUE key will be promoted to become the primary key
              but then the UNIQUE key cannot be marked as IGNORE, so an
              error is thrown
    */
    if (primary_key != MAX_KEY && keyinfo && keyinfo->is_ignored)
    {
        my_error(ER_PK_INDEX_CANT_BE_IGNORED, MYF(0));
        goto err;
    }

    uint add_first_key_parts= 0;
    if (share->use_ext_keys)
    {
      if (primary_key >= MAX_KEY)
        share->set_use_ext_keys_flag(false);
      else
      {
        /* Add primary key to end of all non unique keys */

        KEY *curr_keyinfo= keyinfo, *keyinfo_end= keyinfo+ keys;
        KEY_PART_INFO *first_key_part= keyinfo->key_part;
        uint first_key_parts= keyinfo->user_defined_key_parts;

        /*
          We are skipping the first key (primary key) as it cannot be
          extended
        */
        while (++curr_keyinfo < keyinfo_end)
        {
          uint j;
          if (!(curr_keyinfo->flags & HA_NOSAME))
          {
            KEY_PART_INFO *key_part= (curr_keyinfo->key_part +
                                      curr_keyinfo->user_defined_key_parts);

            /* Extend key with primary key parts */
            for (j= 0;
                 j < first_key_parts &&
                   curr_keyinfo->ext_key_parts < MAX_REF_PARTS;
                 j++)
            {
              uint key_parts= curr_keyinfo->user_defined_key_parts;
              KEY_PART_INFO *curr_key_part= curr_keyinfo->key_part;
              KEY_PART_INFO *curr_key_part_end= curr_key_part+key_parts;

              for ( ; curr_key_part < curr_key_part_end; curr_key_part++)
              {
                if (curr_key_part->fieldnr == first_key_part[j].fieldnr)
                  break;
              }
              if (curr_key_part == curr_key_part_end)
              {
                /* Add primary key part not part of the current index */
                *key_part++= first_key_part[j];
                curr_keyinfo->ext_key_parts++;
                curr_keyinfo->ext_key_part_map|= 1 << j;
              }
            }
            if (j == first_key_parts)
            {
              /* Full primary key added to secondary keys makes it unique */
              curr_keyinfo->ext_key_flags= curr_keyinfo->flags | HA_EXT_NOSAME;
            }
          }
        }
        add_first_key_parts= keyinfo->user_defined_key_parts;

        /*
          If a primary key part is using a partial key, don't use it or any key part after
          it.
        */
        for (i= 0; i < first_key_parts; i++)
        {
          uint fieldnr= keyinfo[0].key_part[i].fieldnr;
          if (share->field[fieldnr-1]->key_length() !=
              keyinfo[0].key_part[i].length)
          {
            add_first_key_parts= i;
            break;
          }
        }
      }
    }

    /* Primary key must be set early as engine may use it in index_flag() */
    share->primary_key= (primary_key < MAX_KEY &&
                         share->keys_in_use.is_set(primary_key) ?
                         primary_key : MAX_KEY);

    key_first_info= keyinfo;
    for (uint key=0 ; key < keys ; key++,keyinfo++)
    {
      uint usable_parts= 0;
      keyinfo->name.str=    share->keynames.type_names[key];
      keyinfo->name.length= strlen(keyinfo->name.str);
      keyinfo->cache_name=
        (uchar*) alloc_root(&share->mem_root,
                            share->table_cache_key.length+
                            keyinfo->name.length + 1);
      if (keyinfo->cache_name)           // If not out of memory
      {
        uchar *pos= keyinfo->cache_name;
        memcpy(pos, share->table_cache_key.str, share->table_cache_key.length);
        memcpy(pos + share->table_cache_key.length, keyinfo->name.str,
               keyinfo->name.length+1);
      }

      if (ext_key_parts > share->key_parts && key)
      {
        KEY_PART_INFO *new_key_part= (keyinfo-1)->key_part +
                                     (keyinfo-1)->ext_key_parts;
        uint add_keyparts_for_this_key= add_first_key_parts;
        uint len_null_byte= 0, ext_key_length= 0;
        Field *field;

        if ((keyinfo-1)->algorithm == HA_KEY_ALG_LONG_HASH)
          new_key_part++; // reserved for the hash value

        /*
          Do not extend the key that contains a component
          defined over the beginning of a field.
	*/
        for (i= 0; i < keyinfo->user_defined_key_parts; i++)
        {
          uint length_bytes= 0;
          uint fieldnr= keyinfo->key_part[i].fieldnr;
          field= share->field[fieldnr-1];

          if (field->null_ptr)
            len_null_byte= HA_KEY_NULL_LENGTH;

          if (keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
            length_bytes= field->key_part_length_bytes();

          ext_key_length+= keyinfo->key_part[i].length + len_null_byte
                            + length_bytes;
          if (field->key_length() != keyinfo->key_part[i].length)
	  {
            add_keyparts_for_this_key= 0;
            break;
          }
        }

        if (add_keyparts_for_this_key)
        {
          for (i= 0; i < add_keyparts_for_this_key; i++)
          {
            uint pk_part_length= key_first_info->key_part[i].store_length;
            if (keyinfo->ext_key_part_map & 1<<i)
            {
              if (ext_key_length + pk_part_length > MAX_DATA_LENGTH_FOR_KEY)
              {
                add_keyparts_for_this_key= i;
                break;
              }
              ext_key_length+= pk_part_length;
            }
          }
        }

        if (add_keyparts_for_this_key < keyinfo->ext_key_parts -
                                        keyinfo->user_defined_key_parts)
        {
          share->ext_key_parts-= keyinfo->ext_key_parts;
          key_part_map ext_key_part_map= keyinfo->ext_key_part_map;
          keyinfo->ext_key_parts= keyinfo->user_defined_key_parts;
          keyinfo->ext_key_flags= keyinfo->flags;
	  keyinfo->ext_key_part_map= 0;
          for (i= 0; i < add_keyparts_for_this_key; i++)
	  {
            if (ext_key_part_map & 1<<i)
	    {
              keyinfo->ext_key_part_map|= 1<<i;
	      keyinfo->ext_key_parts++;
            }
          }
          share->ext_key_parts+= keyinfo->ext_key_parts;
        }
        if (new_key_part != keyinfo->key_part)
	{
          memmove(new_key_part, keyinfo->key_part,
                  sizeof(KEY_PART_INFO) * keyinfo->ext_key_parts);
          keyinfo->key_part= new_key_part;
        }
      }

      key_part= keyinfo->key_part;
      uint key_parts= share->use_ext_keys && key < share->keys
                  ? keyinfo->ext_key_parts : keyinfo->user_defined_key_parts;
      if (keyinfo->algorithm == HA_KEY_ALG_LONG_HASH)
        key_parts++;
      if (keyinfo->algorithm == HA_KEY_ALG_UNDEF) // old .frm
      {
        if (keyinfo->flags & HA_FULLTEXT_legacy)
          keyinfo->algorithm= HA_KEY_ALG_FULLTEXT;
        else if (keyinfo->flags & HA_SPATIAL_legacy)
          keyinfo->algorithm= HA_KEY_ALG_RTREE;
      }
      for (i=0; i < key_parts; key_part++, i++)
      {
        Field *field;
	if (new_field_pack_flag <= 1)
	  key_part->fieldnr= find_field(share->field,
                                        share->default_values,
                                        (uint) key_part->offset,
                                        (uint) key_part->length);
	if (!key_part->fieldnr)
          goto err;

        field= key_part->field= share->field[key_part->fieldnr-1];
        if (Charset::collation_changed_order(share->mysql_version,
                                             field->charset()->number))
          share->incompatible_version|= HA_CREATE_USED_CHARSET;
        key_part->type= field->key_type();

        if (field->invisible > INVISIBLE_USER && !field->vers_sys_field())
          if (keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
            keyinfo->flags |= HA_INVISIBLE_KEY;
        if (field->null_ptr)
        {
          key_part->null_offset=(uint) ((uchar*) field->null_ptr -
                                        share->default_values);
          key_part->null_bit= field->null_bit;
          key_part->store_length+=HA_KEY_NULL_LENGTH;
          keyinfo->flags|=HA_NULL_PART_KEY;
          keyinfo->key_length+= HA_KEY_NULL_LENGTH;
        }

        key_part->key_part_flag|= field->key_part_flag();
        uint16 key_part_length_bytes= field->key_part_length_bytes();
        key_part->store_length+= key_part_length_bytes;
        if (i < keyinfo->user_defined_key_parts)
          keyinfo->key_length+= key_part_length_bytes;

        if (i == 0 && key != primary_key)
          field->flags |= (((keyinfo->flags & HA_NOSAME ||
                            keyinfo->algorithm == HA_KEY_ALG_LONG_HASH) &&
                           (keyinfo->user_defined_key_parts == 1)) ?
                           UNIQUE_KEY_FLAG : MULTIPLE_KEY_FLAG);
        if (i == 0)
          field->key_start.set_bit(key);
        if (field->key_length() == key_part->length &&
            !(field->flags & BLOB_FLAG) && key < share->keys &&
            keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
        {
          if (handler_file->index_flags(key, i, 0) & HA_KEYREAD_ONLY)
          {
            share->keys_for_keyread.set_bit(key);
            /*
              part_of_key is used to check if we can use the field
              as part of covering key (which implies HA_KEYREAD_ONLY).
            */
            field->part_of_key.set_bit(key);
          }
          if (handler_file->index_flags(key, i, 1) & HA_READ_ORDER)
          {
            field->part_of_sortkey.set_bit(key);
            sort_keys_in_use.set_bit(key);
          }

          if (i < keyinfo->user_defined_key_parts)
            field->part_of_key_not_clustered.set_bit(key);
        }
        if (!(key_part->key_part_flag & HA_REVERSE_SORT) &&
            usable_parts == i)
          usable_parts++;			// For FILESORT
        field->flags|= PART_KEY_FLAG;
        if (field->key_length() != key_part->length)
        {
#ifndef TO_BE_DELETED_ON_PRODUCTION
          if (field->type() == MYSQL_TYPE_NEWDECIMAL &&
              keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
          {
            /*
              Fix a fatal error in decimal key handling that causes crashes
              on Innodb. We fix it by reducing the key length so that
              InnoDB never gets a too big key when searching.
              This allows the end user to do an ALTER TABLE to fix the
              error.
            */
            keyinfo->key_length-= (key_part->length - field->key_length());
            key_part->store_length-= (uint16)(key_part->length -
                                              field->key_length());
            key_part->length= (uint16)field->key_length();
            sql_print_error("Found wrong key definition in %s; "
                            "Please do \"ALTER TABLE '%s' FORCE \" to fix it!",
                            share->table_name.str,
                            share->table_name.str);
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                ER_CRASHED_ON_USAGE,
                                "Found wrong key definition in %s; "
                                "Please do \"ALTER TABLE '%s' FORCE\" to fix "
                                "it!",
                                share->table_name.str,
                                share->table_name.str);
            share->crashed= 1;                // Marker for CHECK TABLE
            continue;
          }
#endif
          key_part->key_part_flag|= HA_PART_KEY_SEG;
        }
        if (field->real_maybe_null())
          key_part->key_part_flag|= HA_NULL_PART;
        /*
          Sometimes we can compare key parts for equality with memcmp.
          But not always.
        */
        if (!(key_part->key_part_flag & (HA_BLOB_PART | HA_VAR_LENGTH_PART |
                                         HA_BIT_PART)) &&
            key_part->type != HA_KEYTYPE_FLOAT &&
            key_part->type == HA_KEYTYPE_DOUBLE &&
            keyinfo->algorithm != HA_KEY_ALG_LONG_HASH)
          key_part->key_part_flag|= HA_CAN_MEMCMP;
      }
      keyinfo->usable_key_parts= usable_parts; // Filesort

      set_if_bigger(share->max_key_length,keyinfo->key_length+
                    keyinfo->user_defined_key_parts);
      /*
        MERGE tables do not have unique indexes. But every key could be
        an unique index on the underlying MyISAM table. (Bug #10400)
      */
      if ((keyinfo->flags & HA_NOSAME) ||
          (ha_option & HA_ANY_INDEX_MAY_BE_UNIQUE))
        set_if_bigger(share->max_unique_length,keyinfo->key_length);
    }
    if (primary_key < MAX_KEY &&
	(share->keys_in_use.is_set(primary_key)))
    {
      keyinfo= share->key_info + primary_key;
      /*
	If we are using an integer as the primary key then allow the user to
	refer to it as '_rowid'
      */
      if (keyinfo->user_defined_key_parts == 1)
      {
	Field *field= keyinfo->key_part[0].field;
	if (field && field->result_type() == INT_RESULT)
        {
          /* note that fieldnr here (and rowid_field_offset) starts from 1 */
	  share->rowid_field_offset= keyinfo->key_part[0].fieldnr;
        }
      }
      for (i=0; i < keyinfo->user_defined_key_parts; key_part++, i++)
      {
	Field *field= keyinfo->key_part[i].field;
        field->flags|= PRI_KEY_FLAG;
        /*
          If this field is part of the primary key and all keys contains
          the primary key, then we can use any key to find this column
        */
        if (ha_option & HA_PRIMARY_KEY_IN_READ_INDEX)
        {
          if (field->key_length() == keyinfo->key_part[i].length &&
              !(field->flags & BLOB_FLAG))
          {
            field->part_of_key= share->keys_in_use;
            field->part_of_sortkey= sort_keys_in_use;
          }
        }
      }
      DBUG_ASSERT(share->primary_key == primary_key);
    }
    else
    {
      DBUG_ASSERT(share->primary_key == MAX_KEY);
    }
  }
  if (new_field_pack_flag <= 1)
  {
    /* Old file format with default as not null */
    uint null_length= (share->null_fields+7)/8;
    bfill(share->default_values + (null_flags - (uchar*) record),
          null_length, 255);
  }

  set_overlapped_keys();

  /* Handle virtual expressions */
  if (vcol_screen_length && share->frm_version >= FRM_VER_EXPRESSSIONS)
  {
    uchar *vcol_screen_end= vcol_screen_pos + vcol_screen_length;

    /* Skip header */
    vcol_screen_pos+= FRM_VCOL_NEW_BASE_SIZE;
    share->vcol_defs.str+= FRM_VCOL_NEW_BASE_SIZE;
    share->vcol_defs.length-= FRM_VCOL_NEW_BASE_SIZE;

    /*
      Read virtual columns, default values and check constraints
      See pack_expression() for how data is stored
    */
    while (vcol_screen_pos < vcol_screen_end)
    {
      Virtual_column_info *vcol_info;
      uint type=         (uint) vcol_screen_pos[0];
      uint field_nr=     uint2korr(vcol_screen_pos+1);
      uint expr_length=  uint2korr(vcol_screen_pos+3);
      uint name_length=  (uint) vcol_screen_pos[5];

      if (!(vcol_info=   new (&share->mem_root) Virtual_column_info()))
        goto err;

      /* The following can only be true for check_constraints */

      if (field_nr != UINT_MAX16)
      {
        DBUG_ASSERT(field_nr < share->fields);
        reg_field= share->field[field_nr];
      }
      else
      {
        reg_field= 0;
        DBUG_ASSERT(name_length);
      }

      vcol_screen_pos+= FRM_VCOL_NEW_HEADER_SIZE;
      vcol_info->set_vcol_type((enum_vcol_info_type) type);
      if (name_length)
      {
        vcol_info->name.str= strmake_root(&share->mem_root,
                                          (char*)vcol_screen_pos, name_length);
        vcol_info->name.length= name_length;
      }
      else
        vcol_info->name= reg_field->field_name;
      vcol_screen_pos+= name_length + expr_length;

      switch (type) {
      case VCOL_GENERATED_VIRTUAL:
      {
        uint recpos;
        reg_field->vcol_info= vcol_info;
        share->virtual_fields++;
        share->stored_fields--;
        if (reg_field->flags & BLOB_FLAG)
          share->virtual_not_stored_blob_fields++;
        if (reg_field->flags & PART_KEY_FLAG)
          vcol_info->set_vcol_type(VCOL_GENERATED_VIRTUAL_INDEXED);
        /* Correct stored_rec_length as non stored fields are last */
        recpos= (uint) (reg_field->ptr - record);
        if (share->stored_rec_length >= recpos)
          share->stored_rec_length= recpos-1;
        break;
      }
      case VCOL_GENERATED_STORED:
        DBUG_ASSERT(!reg_field->vcol_info);
        reg_field->vcol_info= vcol_info;
        share->virtual_fields++;
        break;
      case VCOL_DEFAULT:
        DBUG_ASSERT(!reg_field->default_value);
        reg_field->default_value=    vcol_info;
        share->default_expressions++;
        break;
      case VCOL_CHECK_FIELD:
        DBUG_ASSERT(!reg_field->check_constraint);
        reg_field->check_constraint= vcol_info;
        share->field_check_constraints++;
        break;
      case VCOL_CHECK_TABLE:
        *(table_check_constraints++)= vcol_info;
        break;
      }
    }
  }
  DBUG_ASSERT((uint) (table_check_constraints - share->check_constraints) ==
              (uint) (share->table_check_constraints -
                      share->field_check_constraints));

  if (options.str)
  {
    DBUG_ASSERT(options.length);
    if (engine_table_options_frm_read(options.str, options.length, share))
      goto err;
  }
  if (parse_engine_table_options(thd, handler_file->partition_ht(), share))
    goto err;

  if (share->hlindexes())
  {
    DBUG_ASSERT(share->hlindexes() == 1);
    keyinfo= share->key_info + share->keys;
    if (parse_option_list(thd, &keyinfo->option_struct, &keyinfo->option_list,
                          mhnsw_index_options, TRUE, thd->mem_root))
      goto err;
  }

  if (share->found_next_number_field)
  {
    reg_field= *share->found_next_number_field;
    if ((int) (share->next_number_index= (uint)
	       find_ref_key(share->key_info, keys,
                            share->default_values, reg_field,
			    &share->next_number_key_offset,
                            &share->next_number_keypart)) < 0)
      goto err; // Wrong field definition
    reg_field->flags |= AUTO_INCREMENT_FLAG;
  }
  else
    share->next_number_index= MAX_KEY;

  if (share->blob_fields)
  {
    Field **ptr;
    uint k, *save;

    /* Store offsets to blob fields to find them fast */
    if (!(share->blob_field= save=
	  (uint*) alloc_root(&share->mem_root,
                             (uint) (share->blob_fields* sizeof(uint)))))
      goto err;
    for (k=0, ptr= share->field ; *ptr ; ptr++, k++)
    {
      if ((*ptr)->flags & BLOB_FLAG)
	(*save++)= k;
    }
  }

  /*
    the correct null_bytes can now be set, since bitfields have been taken
    into account
  */
  share->null_bytes= (uint)(null_pos - (uchar*) null_flags +
                      (null_bit_pos + 7) / 8);
  share->last_null_bit_pos= null_bit_pos;
  share->null_bytes_for_compare= null_bits_are_used ? share->null_bytes : 0;
  share->can_cmp_whole_record= (share->blob_fields == 0 &&
                                share->varchar_fields == 0);

  share->column_bitmap_size= bitmap_buffer_size(share->fields);

  bitmap_count= 1;
  if (share->table_check_constraints)
  {
    feature_check_constraint++;
    if (!(share->check_set= (MY_BITMAP*)
          alloc_root(&share->mem_root, sizeof(*share->check_set))))
      goto err;
    bitmap_count++;
  }
  if (!(bitmaps= (my_bitmap_map*) alloc_root(&share->mem_root,
                                             share->column_bitmap_size *
                                             bitmap_count)))
    goto err;
  my_bitmap_init(&share->all_set, bitmaps, share->fields);
  bitmap_set_all(&share->all_set);
  if (share->check_set)
  {
    /*
      Bitmap for fields used by CHECK constraint. Will be filled up
      at first usage of table.
    */
    my_bitmap_init(share->check_set,
                   (my_bitmap_map*) ((uchar*) bitmaps +
                                     share->column_bitmap_size),
                   share->fields);
    bitmap_clear_all(share->check_set);
  }

#ifndef DBUG_OFF
  if (use_hash)
    (void) my_hash_check(&share->name_hash);
#endif

  share->db_plugin= se_plugin;
  delete handler_file;

  share->error= OPEN_FRM_OK;
  thd->status_var.opened_shares++;
  thd->mem_root= old_root;
  my_afree(interval_unescaped);
  DBUG_RETURN(0);

err:
  if (frm_created)
  {
    char path[FN_REFLEN+1];
    strxnmov(path, FN_REFLEN, normalized_path.str, reg_ext, NullS);
    my_delete(path, MYF(0));
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (par_image)
    {
      strxnmov(path, FN_REFLEN, normalized_path.str, PAR_EXT, NullS);
      my_delete(path, MYF(0));
    }
#endif
  }
  share->db_plugin= NULL;
  share->error= OPEN_FRM_CORRUPTED;
  share->open_errno= my_errno;
  delete handler_file;
  plugin_unlock(0, se_plugin);
  my_hash_free(&share->name_hash);

  if (!thd->is_error())
    open_table_error(share, OPEN_FRM_CORRUPTED, share->open_errno);

  thd->mem_root= old_root;
  my_afree(interval_unescaped);
  DBUG_RETURN(HA_ERR_NOT_A_TABLE);
}

