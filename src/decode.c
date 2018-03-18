/*****************************************************************************/
/*  LibreDWG - free implementation of the DWG file format                    */
/*                                                                           */
/*  Copyright (C) 2009 Free Software Foundation, Inc.                        */
/*                                                                           */
/*  This library is free software, licensed under the terms of the GNU       */
/*  General Public License as published by the Free Software Foundation,     */
/*  either version 3 of the License, or (at your option) any later version.  */
/*  You should have received a copy of the GNU General Public License        */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>.    */
/*****************************************************************************/

/*
 * decode.c: decoding functions
 * written by Felipe Castro
 * modified by Felipe Corrêa da Silva Sances
 * modified by Rodrigo Rodrigues da Silva
 * modified by Till Heuschmann
 * modified by Reini Urban
 */

#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "bits.h"
#include "dwg.h"
#include "decode.h"
#include "print.h"

/* The logging level for the read (decode) path.  */
unsigned int loglevel;
/* the current version per spec block */
static unsigned int cur_ver = 0;

#ifdef USE_TRACING
/* This flag means we have checked the environment variable
   LIBREDWG_TRACE and set `loglevel' appropriately.  */
static bool env_var_checked_p;

#define DWG_LOGLEVEL loglevel
#endif  /* USE_TRACING */

#include "logging.h"
#include "dec_macros.h"

#define REFS_PER_REALLOC 100

/*--------------------------------------------------------------------------------
 * Private functions
 */

enum RES_BUF_VALUE_TYPE
{
  VT_INVALID = 0,
  VT_STRING = 1,
  VT_POINT3D = 2,
  VT_REAL = 3,
  VT_INT16 = 4,
  VT_INT32 = 5,
  VT_INT8 = 6,
  VT_BINARY = 7,
  VT_HANDLE = 8,
  VT_OBJECTID = 9,
  VT_BOOL = 10
};

static enum RES_BUF_VALUE_TYPE
get_base_value_type(short gc);

static void
dwg_decode_add_object(Dwg_Data * dwg, Bit_Chain * dat,
    long unsigned int address);

static Dwg_Object *
dwg_resolve_handle(Dwg_Data* dwg, unsigned long int handle);

static void
dwg_decode_header_variables(Bit_Chain* dat, Dwg_Data * dwg);

static int
resolve_objectref_vector(Dwg_Data * dwg);

static int
decode_R13_R15(Bit_Chain* dat, Dwg_Data * dwg);

static int
decode_R2004(Bit_Chain* dat, Dwg_Data * dwg);

static int
decode_R2007(Bit_Chain* dat, Dwg_Data * dwg);

static Dwg_Resbuf*
dwg_decode_xdata(Bit_Chain * dat, int size);

static int
dwg_decode_object(Bit_Chain * dat, Dwg_Object_Object * obj);

/*--------------------------------------------------------------------------------
 * Imported functions
 */

int
read_r2007_meta_data(Bit_Chain *dat, Dwg_Data *dwg);

/*--------------------------------------------------------------------------------
 * Public variables
 */
long unsigned int ktl_lastaddress;

/*--------------------------------------------------------------------------------
 * Public function definitions
 */

// returns 0 on success
int
dwg_decode_data(Bit_Chain * dat, Dwg_Data * dwg)
{
  int i;
  char version[7];
  dwg->num_object_refs = 0;
  dwg->num_layers = 0;
  dwg->num_entities = 0;
  dwg->num_objects = 0;
  dwg->num_classes = 0;

#ifdef USE_TRACING
  /* Before starting, set the logging level, but only do so once.  */
  if (! env_var_checked_p)
    {
      char *probe = getenv ("LIBREDWG_TRACE");

      if (probe)
        loglevel = atoi (probe);
      env_var_checked_p = true;
    }
#endif  /* USE_TRACING */

  /* Version */
  dat->byte = 0;
  dat->bit = 0;
  strncpy(version, (const char *)dat->chain, 6);
  version[6] = '\0';

  dwg->header.version = 0;
  for (i=0; i<R_AFTER; i++)
    {
      if (!strcmp(version, version_codes[(Dwg_Version_Type)i])) {
        dwg->header.version = (Dwg_Version_Type)i;
        break;
      }
    }
  if (dwg->header.version == 0)
    {
      LOG_ERROR("Invalid or unimplemented version code! "
        "This file's version code is: %s\n", version)
      return -1;
    }
  dat->version = dwg->header.version;
  LOG_INFO("This file's version code is: %s\n", version)
  dwg->header.from_version = 0;
  dat->from_version = 0;

#define WE_CAN "This version of LibreDWG is only capable of safely decoding version R13-R2000 (code: AC1012-AC1015) dwg-files.\n"

  PRE(R_13)
    {
      LOG_ERROR(WE_CAN "This file's version code is: %s", version)
      //return decode_preR13(dat, dwg); //TODO rurban: search my old hard drives
      return -1;
    }

  VERSION(R_2000)
    {
      return decode_R13_R15(dat, dwg);
    }

  VERSION(R_2004)
    {
      LOG_WARN(WE_CAN "This file's version code is: %s\n"
          "Support for this version is still experimental.", version)
      return decode_R2004(dat, dwg);
    }

  SINCE(R_2007)
    {
      LOG_WARN(WE_CAN "This file's version code is: %s\n"
               "Support for this version is still experimental.\n"
               "It will probably crash and/or give you invalid output.", version)
      return decode_R2007(dat, dwg);
    }

  //This line should not be reached!
  LOG_ERROR(
      "LibreDWG does not support this version: %s.", version)
  return -1;
}

static int
decode_R13_R15(Bit_Chain* dat, Dwg_Data * dwg)
{
  Dwg_Object *obj = NULL;
  unsigned int section_size = 0;
  unsigned char sgdc[2];
  unsigned int ckr, ckr2;
  long unsigned int size;
  long unsigned int lasta;
  long unsigned int maplasta;
  long unsigned int duabyte;
  long unsigned int object_begin;
  long unsigned int object_end;
  long unsigned int pvz;
  unsigned int j, k;

  {
    int i;
    struct Dwg_Header *_obj = &dwg->header;
    dat->byte = 0x06;

    #include "header.spec"
  }

  /* Section Locator Records 0x15 */
  assert(dat->byte == 0x15);
  dwg->header.num_sections = bit_read_RL(dat);
  if (!dwg->header.num_sections) //ODA writes zeros.
    dwg->header.num_sections = 6;

  // So far seen 3-6 sections. Most emit only 3-5 sections.
  dwg->header.section = (Dwg_Section*) malloc(sizeof(Dwg_Section)
      * dwg->header.num_sections);
  /* section 0: header vars
   *         1: class section
   *         2: object map
   *         3: (R13 c3 and later): 2nd header (special table no sentinels)
   *         4: optional: MEASUREMENT
   */
  for (j = 0; j < dwg->header.num_sections; j++)
    {
      dwg->header.section[j].number  = bit_read_RC(dat);
      dwg->header.section[j].address = bit_read_RL(dat);
      dwg->header.section[j].size    = bit_read_RL(dat);
    }

  // Check CRC-on
  ckr = bit_calc_CRC(0xc0c1, dat->chain, dat->byte);
  ckr2 = bit_read_RS(dat);
  if (ckr != ckr2)
    {
      printf("Error: Header CRC mismatch %d <=> %d\n", ckr, ckr2);
      /*return 1;*/
      /* The CRC depends on num_sections. XOR result with
         3: 0xa598
         4: 0x8101
         5: 0x3cc4
         6: 0x8461
      */
    }

  if (bit_search_sentinel(dat, dwg_sentinel(DWG_SENTINEL_HEADER_END)))
    LOG_TRACE("\n=======> HEADER (end): %8X\n", (unsigned int) dat->byte)

  /*-------------------------------------------------------------------------
   * Section 5 AuxHeader
   * R2000+, mostly redundant file header information
   */

  if (dwg->header.num_sections == 6)
    {
      int i;
      struct Dwg_AuxHeader* _obj = &dwg->auxheader;
      obj = NULL;

      LOG_TRACE("\n=======> AuxHeader: %8X\n",
              (unsigned int) dwg->header.section[5].address)
      LOG_TRACE("         AuxHeader (end): %8X\n",
              (unsigned int) (dwg->header.section[5].address
                  + dwg->header.section[5].size))
      dat->byte = dwg->header.section[5].address;

      #include "auxheader.spec"

      /*
      dwg->AuxHeader.size = DWG_AUXHEADER_SIZE;
      dwg->AuxHeader.byte = dwg->AuxHeader.bit = 0;
      dwg->AuxHeader.chain = (unsigned char*)malloc(dwg->AuxHeader.size);
      memcpy(dwg->AuxHeader.chain, &dat->chain[dat->byte], dwg->AuxHeader.size);
      */
      //bit_explore_chain ((Bit_Chain *) &dwg->unknown1, dwg->unknown1.size);
      //bit_print ((Bit_Chain *) &dwg->unknown1, dwg->unknown1.size);
    }

  /*-------------------------------------------------------------------------
   * Picture (Pre-R13C3?)
   */

  if (bit_search_sentinel(dat, dwg_sentinel(DWG_SENTINEL_PICTURE_BEGIN)))
    {
      unsigned long int start_address;

      dat->bit = 0;
      start_address = dat->byte;
      LOG_TRACE("\n=======> PICTURE: %8X\n",
            (unsigned int) start_address - 16)
      if (bit_search_sentinel(dat, dwg_sentinel(DWG_SENTINEL_PICTURE_END)))
        {
          LOG_TRACE("         PICTURE (end): %8X\n",
                (unsigned int) dat->byte)
          dwg->picture.size = (dat->byte - 16) - start_address;
          dwg->picture.chain = (unsigned char *) malloc(dwg->picture.size);
          memcpy(dwg->picture.chain, &dat->chain[start_address],
              dwg->picture.size);
        }
      else
        dwg->picture.size = 0;
    }

  /*-------------------------------------------------------------------------
   * Header Variables, section 0
   */

  LOG_INFO("\n=======> Header Variables: %8X\n",
          (unsigned int) dwg->header.section[0].address)
  LOG_INFO("         Header Variables (end): %8X\n",
          (unsigned int) (dwg->header.section[0].address
              + dwg->header.section[0].size))
  dat->byte = dwg->header.section[0].address + 16;
  pvz = bit_read_RL(dat);
  LOG_TRACE("         Length: %lu\n", pvz)

  dat->bit = 0;
  dwg_decode_header_variables(dat, dwg);

  // Check CRC-on
  dat->bit = 0;
  ckr = dwg->header_vars.CRC;
  ckr2 = bit_calc_CRC(0xc0c1, dat->chain + dwg->header.section[0].address + 16,
                      dwg->header.section[0].size - 34);
  if (ckr != ckr2)
    {
      printf("Error: Section[%d] CRC mismatch %d <=> %d\n",
             dwg->header.section[0].number, ckr, ckr2);
      return -1;
    }

  /*-------------------------------------------------------------------------
   * Classes, section 1
   */
  LOG_INFO("\n=======> CLASS: %8lX\n",
           (long)dwg->header.section[1].address)
  LOG_INFO("         CLASS (end): %8lX\n",
           (long)(dwg->header.section[1].address
              + dwg->header.section[1].size))
  dat->byte = dwg->header.section[1].address + 16;
  dat->bit = 0;

  size = bit_read_RL(dat);
  lasta = dat->byte + size;
  LOG_TRACE("         Length: %lu\n", size);

  /* read the classes
   */
  dwg->dwg_ot_layout = 0;
  dwg->num_classes = 0;
  j = 0;
  do
    {
      unsigned int idc;

      idc = dwg->num_classes;
      if (idc == 0)
        dwg->dwg_class = (Dwg_Class *) malloc(sizeof(Dwg_Class));
      else
        dwg->dwg_class = (Dwg_Class *) realloc(dwg->dwg_class, (idc + 1)
            * sizeof(Dwg_Class));

      dwg->dwg_class[idc].number = bit_read_BS(dat);
      dwg->dwg_class[idc].version = bit_read_BS(dat);
      dwg->dwg_class[idc].appname = bit_read_TV(dat);
      dwg->dwg_class[idc].cppname = bit_read_TV(dat);
      dwg->dwg_class[idc].dxfname = bit_read_TV(dat);
      dwg->dwg_class[idc].wasazombie = bit_read_B(dat);
      dwg->dwg_class[idc].item_class_id = bit_read_BS(dat);

      if (strcmp((const char *)dwg->dwg_class[idc].dxfname, "LAYOUT") == 0)
        dwg->dwg_ot_layout = dwg->dwg_class[idc].number;

      dwg->num_classes++;
      /*
      if (dwg->num_classes > 100)
	{
	  fprintf(stderr, "number of classes is greater than 100. TODO: Why should we stop here?\n");
	  break;//TODO: Why?!
	}
      */
    }
  while (dat->byte < (lasta - 1));

  // Check CRC-on
  dat->byte = dwg->header.section[1].address + dwg->header.section[1].size - 18;
  dat->bit = 0;
  ckr = bit_read_RS(dat);
  ckr2 = bit_calc_CRC(0xc0c1, dat->chain + dwg->header.section[1].address + 16,
                      dwg->header.section[1].size - 34);
  if (ckr != ckr2)
    {
      printf("Error: Section[%d] CRC mismatch %d <=> %d\n",
             dwg->header.section[1].number, ckr, ckr2);
      return -1;
    }

  dat->byte += 16;
  pvz = bit_read_RL(dat); // Unknown bitlong inter class and object
  LOG_TRACE("@ %lu RL: 0x%#lX\n", dat->byte - 4, pvz)
  LOG_INFO("Number of classes read: %u\n", dwg->num_classes)

  /*-------------------------------------------------------------------------
   * Object-map, section 2
   */

  dat->byte = dwg->header.section[2].address;
  dat->bit = 0;

  maplasta = dat->byte + dwg->header.section[2].size; // 4
  dwg->num_objects = 0;
  object_begin = dat->size;
  object_end = 0;
  do
    {
      long unsigned int last_address;
      long unsigned int last_handle;
      long unsigned int previous_address = 0;

      duabyte = dat->byte;
      sgdc[0] = bit_read_RC(dat);
      sgdc[1] = bit_read_RC(dat);
      section_size = (sgdc[0] << 8) | sgdc[1];
      //LOG_TRACE("section_size: %u\n", section_size)
      if (section_size > 2035)
        {
          LOG_ERROR("Object-map section size greater than 2035!")
          return -1;
        }

      last_handle = 0;
      last_address = 0;
      while (dat->byte - duabyte < section_size)
        {
#if 0
          long unsigned int kobj;
#endif
          long int pvztkt;
          long int pvzadr;
          previous_address = dat->byte;
          pvztkt = bit_read_MC(dat);
          last_handle += pvztkt;
          pvzadr = bit_read_MC(dat);
          last_address += pvzadr;
          LOG_TRACE("Idc: %li\t", dwg->num_objects)
          LOG_TRACE("Handle: %li\tAddress: %li", pvztkt, pvzadr)
          //}
          if (dat->byte == previous_address)
            break;
          //if (dat->byte - duabyte >= seksize)
          //break;

          if (object_end < last_address)
            object_end = last_address;
          if (object_begin > last_address)
            object_begin = last_address;

          dwg_decode_add_object(dwg, dat, last_address);
#if 0
          kobj = dwg->num_objects;
          if (dwg->num_objects > kobj)
            dwg->object[dwg->num_objects - 1].handle.value = lastahandle;
          //TODO: blame Juca
#endif
        }
      if (dat->byte == previous_address)
        break;

      // CRC on
      if (dat->bit > 0)
        {
          dat->byte += 1;
          dat->bit = 0;
        }

      sgdc[0] = bit_read_RC(dat);
      sgdc[1] = bit_read_RC(dat);
      ckr = (sgdc[0] << 8) | sgdc[1];

      ckr2 = bit_calc_CRC(0xc0c1, dat->chain + duabyte, section_size);
      if (ckr != ckr2)
        {
          printf("Error: Section CRC mismatch %d <=> %d\n", ckr, ckr2);
          return -1;
        }

      if (dat->byte >= maplasta)
        break;
    }
  while (section_size > 2);

  LOG_INFO("Num objects: %lu\n", dwg->num_objects)
  LOG_INFO("\n=======> Object Data: %8X\n", (unsigned int) object_begin)
  dat->byte = object_end;
  object_begin = bit_read_MS(dat);
  LOG_INFO("         Object Data (end): %8X\n",
      (unsigned int) (object_end + object_begin+ 2))

  /*
   dat->byte = dwg->header.section[2].address - 2;
   antckr = bit_read_CRC (dat); // Unknown bitdouble inter object data and object map
   LOG_TRACE("Address: %08X / Content: 0x%04X", dat->byte - 2, antckr)

   // check CRC-on
   antckr = 0xC0C1;
   do
   {
     duabyte = dat->byte;
     sgdc[0] = bit_read_RC (dat);
     sgdc[1] = bit_read_RC (dat);
     section_size = (sgdc[0] << 8) | sgdc[1];
     section_size -= 2;
     dat->byte += section_size;
     ckr = bit_read_CRC (dat);
     dat->byte -= 2;
     bit_write_CRC (dat, duabyte, antckr);
     dat->byte -= 2;
     ckr2 = bit_read_CRC (dat);
     if (loglevel) fprintf (stderr, "Read: %X\nCreated: %X\t SEMO: %X\n", ckr, ckr2, antckr);
     //antckr = ckr;
   } while (section_size > 0);
   */
  LOG_INFO("\n=======> Object Map: %8X\n",
          (unsigned int) dwg->header.section[2].address)
  LOG_INFO("         Object Map (end): %8X\n",
          (unsigned int) (dwg->header.section[2].address
              + dwg->header.section[2].size))

  /*-------------------------------------------------------------------------
   * Second header, section 3
   */

  if (bit_search_sentinel(dat, dwg_sentinel(DWG_SENTINEL_SECOND_HEADER_BEGIN)))
    {
      int i;
      BITCODE_RC sig, sig2;
      long unsigned int pvzadr;

      LOG_INFO("\n=======> Second Header: %8X\n",
          (unsigned int) dat->byte-16)
      pvzadr = dat->byte;
      LOG_TRACE("pvzadr: %lx\n", pvzadr)

      pvz = bit_read_RL(dat);
      LOG_TRACE("Size: %lu\n", pvz)

      pvz = bit_read_BL(dat);
      LOG_TRACE("Begin address: %8lX\n", pvz)

      //AC1012, AC1014 or AC1015
      for (i = 0; i < 6; i++)
        {
          unsigned char c;
          sig = bit_read_RC(dat);
          c = (sig >= ' ' && (unsigned char)sig < 128) ? sig : '.';
          LOG_TRACE("%c", c)
        }

      //LOG_TRACE("Null?:")
      for (i = 0; i < 5; i++) // 6 if is older...
        {
          sig = bit_read_RC(dat);
          //LOG_TRACE(" 0x%02X", sig)
        }

      //LOG_TRACE"4 null bits?: ")
      for (i = 0; i < 4; i++)
        {
          sig = bit_read_B(dat);
          //LOG_TRACE(" %c", sig ? '1' : '0')
        }

      //LOG_TRACE(stderr, "Chain?: ")
      for (i = 0; i < 6; i++)
        {
          dwg->second_header.unknown[i] = bit_read_RC(dat);
          //LOG_TRACE(" 0x%02X", dwg->second_header.unknown[i])
        }
      if (dwg->second_header.unknown[3] != 0x78
          || dwg->second_header.unknown[5] != 0x06)
        sig = bit_read_RC(dat); // To compensate in the event of a contingent
                                // additional zero not read previously

      //puts("");
      for (i = 0; i < 6; i++)
        {
          sig = bit_read_RC(dat);
          //if (loglevel) fprintf (stderr, "[%u]\n", sig);
          pvz = bit_read_BL(dat);
          //if (loglevel) fprintf (stderr, " Address: %8X\n", pvz);
          pvz = bit_read_BL(dat);
          //if (loglevel) fprintf (stderr, "  Size: %8X\n", pvz);
        }

      bit_read_BS(dat);
      //if (loglevel) fprintf (stderr, "\n14 --------------");
      for (i = 0; i < 14; i++)
        {
          sig2 = bit_read_RC(dat);
          dwg->second_header.handlerik[i].size = sig2;
          //if (loglevel) fprintf (stderr, "\nSize: %u\n", sig2);
          sig = bit_read_RC(dat);
          //if (loglevel) fprintf (stderr, "\t[%u]\n", sig);
          //if (loglevel) fprintf (stderr, "\tChain:");
          for (j = 0; j < (unsigned)sig2; j++)
            {
              sig = bit_read_RC(dat);
              dwg->second_header.handlerik[i].chain[j] = sig;
              //if (loglevel) fprintf (stderr, " %02X", sig);
            }
        }

      // Check CRC-on
      ckr = bit_read_CRC(dat);
#if 0
      puts ("");
      for (i = 0; i != 0xFFFF; i++)
        {
          dat->byte -= 2;
          bit_write_CRC (dat, pvzadr, i);
          dat->byte -= 2;
          ckr2 = bit_read_CRC (dat);
          if (ckr == ckr2)
            {
              printf("Warning: CRC mismatch %d <=> %d\n",
                     ckr, ckr2);
              break;
            }
          if (loglevel) {
            fprintf (stderr, " Unknown RL 1: %08X\n", bit_read_RL (dat));
            fprintf (stderr, " Unknown RL 2: %08X\n", bit_read_RL (dat));
          }
        }
#endif

      if (bit_search_sentinel(dat, dwg_sentinel(
          DWG_SENTINEL_SECOND_HEADER_END)))
        LOG_INFO("         Second Header (end): %8X\n", (unsigned int) dat->byte)
    }

  /*-------------------------------------------------------------------------
   * Section 4: MEASUREMENT
   */

  LOG_INFO("\n=======> Unknown 4: %8X\n",
          (unsigned int) dwg->header.section[4].address)
  LOG_INFO("         Unknown 4 (end): %8X\n",
          (unsigned int) (dwg->header.section[4].address
              + dwg->header.section[4].size))
  dat->byte = dwg->header.section[4].address;
  dat->bit = 0;
  dwg->measurement = bit_read_RL(dat);

  LOG_TRACE("         Size bytes :\t%lu\n", dat->size)

  //step II of handles parsing: resolve pointers from handle value
  //XXX: move this somewhere else
  LOG_TRACE("\n\nResolving pointers from ObjectRef vector.\n")
  return resolve_objectref_vector(dwg);
}

static int
resolve_objectref_vector(Dwg_Data * dwg)
{
  long unsigned int i;
  Dwg_Object * obj;

  for (i = 0; i < dwg->num_object_refs; i++)
    {
      LOG_TRACE("\n==========\n")
      LOG_TRACE("-objref: HANDLE(%d.%d.%lu) Absolute:%lu\n",
                dwg->object_ref[i]->handleref.code,
                dwg->object_ref[i]->handleref.size,
                dwg->object_ref[i]->handleref.value,
                dwg->object_ref[i]->absolute_ref)

      //look for object
      obj = dwg_resolve_handle(dwg, dwg->object_ref[i]->absolute_ref);

      if (obj)
        {
          LOG_TRACE("-found:  HANDLE(%d.%d.%lu)\n",
              obj->handle.code,
              obj->handle.size,
              obj->handle.value)
        }

      //assign found pointer to objectref vector
      dwg->object_ref[i]->obj = obj;

      if (DWG_LOGLEVEL >= DWG_LOGLEVEL_HANDLE)
        {
          if (obj)
            dwg_print_object(obj);
          else
            LOG_WARN("Null object pointer: object_ref[%lu]", i)
        }
    }
  return dwg->num_object_refs ? 0 : 1;
}

/* R2004 Literal Length
 */
static int
read_literal_length(Bit_Chain* dat, unsigned char *opcode)
{
  int total = 0;
  unsigned char byte = bit_read_RC(dat);

  *opcode = 0x00;

  if (byte >= 0x01 && byte <= 0x0F)
    return byte + 3;
  else if (byte == 0)
    {
      total = 0x0F;
      while ((byte = bit_read_RC(dat)) == 0x00)
        {
          total += 0xFF;
        }
      return total + byte + 3;
    }
  else if (byte & 0xF0)
    *opcode = byte;

  return 0;
}

/* R2004 Long Compression Offset
 */
static int
read_long_compression_offset(Bit_Chain* dat)
{
  int total = 0;
  unsigned char byte = bit_read_RC(dat);
  if (byte == 0)
    {
      total = 0xFF;
      while ((byte = bit_read_RC(dat)) == 0x00)
        total += 0xFF;
    }
  return total + byte;
}

/* R2004 Two Byte Offset
 */
static int
read_two_byte_offset(Bit_Chain* dat, int* lit_length)
{
  int offset;
  unsigned char firstByte = bit_read_RC(dat);
  unsigned char secondByte = bit_read_RC(dat);
  offset = (firstByte >> 2) | (secondByte << 6);
  *lit_length = (firstByte & 0x03);
  return offset;
}

/* Decompresses a system section of a 2004 DWG file
 */
static int
decompress_R2004_section(Bit_Chain* dat, char *decomp,
                         unsigned long int comp_data_size)
{
  int lit_length, i;
  int comp_offset, comp_bytes;
  unsigned char opcode1 = 0, opcode2;
  long unsigned int start_byte = dat->byte;
  char *src, *dst = decomp;

  // length of the first sequence of uncompressed or literal data.
  lit_length = read_literal_length(dat, &opcode1);

  for (i = 0; i < lit_length; ++i)
    *dst++ = bit_read_RC(dat);

  opcode1 = 0x00;
  while (dat->byte - start_byte < comp_data_size)
    {
      if (opcode1 == 0x00)
        opcode1 = bit_read_RC(dat);

      if (opcode1 >= 0x40)
        {
          comp_bytes = ((opcode1 & 0xF0) >> 4) - 1;
          opcode2 = bit_read_RC(dat);
          comp_offset = (opcode2 << 2) | ((opcode1 & 0x0C) >> 2);

          if (opcode1 & 0x03)
            {
              lit_length = (opcode1 & 0x03);
              opcode1  = 0x00;
            }
          else
            lit_length = read_literal_length(dat, &opcode1);
        }
      else if (opcode1 >= 0x21 && opcode1 <= 0x3F)
        {
          comp_bytes  = opcode1 - 0x1E;
          comp_offset = read_two_byte_offset(dat, &lit_length);

          if (lit_length != 0)
            opcode1 = 0x00;
          else
            lit_length = read_literal_length(dat, &opcode1);
        }
      else if (opcode1 == 0x20)
        {
          comp_bytes  = read_long_compression_offset(dat) + 0x21;
          comp_offset = read_two_byte_offset(dat, &lit_length);

          if (lit_length != 0)
            opcode1 = 0x00;
          else
            lit_length = read_literal_length(dat, &opcode1);
        }
      else if (opcode1 >= 0x12 && opcode1 <= 0x1F)
        {
          comp_bytes  = (opcode1 & 0x0F) + 2;
          comp_offset = read_two_byte_offset(dat, &lit_length) + 0x3FFF;

          if (lit_length != 0)
            opcode1 = 0x00;
          else
            lit_length = read_literal_length(dat, &opcode1);
        }
      else if (opcode1 == 0x10)
        {
          comp_bytes  = read_long_compression_offset(dat) + 9;
          comp_offset = read_two_byte_offset(dat, &lit_length) + 0x3FFF;

          if (lit_length != 0)
            opcode1 = 0x00;
          else
            lit_length = read_literal_length(dat, &opcode1);
        }
      else if (opcode1 == 0x11)
          break;     // Terminates the input stream, everything is ok!
      else
          return 1;  // error in input stream

      //LOG_TRACE("got compressed data %d\n",comp_bytes)
      // copy "compressed data"
      src = dst - comp_offset - 1;
      assert(src > decomp);
      for (i = 0; i < comp_bytes; ++i)
        *dst++ = *src++;

      // copy "literal data"
      //LOG_TRACE("got literal data %d\n",lit_length)
      for (i = 0; i < lit_length; ++i)
        *dst++ = bit_read_RC(dat);
    }

  return 0;  // Success
}

/* Read R2004 Section Map
 * The Section Map is a vector of number, size, and address triples used
 * to locate the sections in the file.
 */
static void
read_R2004_section_map(Bit_Chain* dat, Dwg_Data * dwg,
                       unsigned long int comp_data_size,
                       unsigned long int decomp_data_size)
{
  char *decomp, *ptr;
  int i;
  int section_address;
  int bytes_remaining;

  dwg->header.num_sections = 0;
  dwg->header.section = 0;

  // allocate memory to hold decompressed data
  decomp = (char *)malloc(decomp_data_size * sizeof(char));
  if (decomp == 0)
    return;   // No memory

  decompress_R2004_section(dat, decomp, comp_data_size);

  LOG_TRACE("\n#### 2004 Section Map fields ####\n")

  section_address = 0x100;  // starting address
  i = 0;
  bytes_remaining = decomp_data_size;
  ptr = decomp;
  dwg->header.num_sections = 0;

  while(bytes_remaining)
    {
      if (dwg->header.num_sections==0)
        dwg->header.section = (Dwg_Section*) malloc(sizeof(Dwg_Section));
      else
        dwg->header.section = (Dwg_Section*) realloc(dwg->header.section,
                                                     sizeof(Dwg_Section) * (dwg->header.num_sections+1));

      dwg->header.section[i].number  = *((int*)ptr);
      dwg->header.section[i].size    = *((int*)ptr+1);
      dwg->header.section[i].address = section_address;
      section_address += dwg->header.section[i].size;
      bytes_remaining -= 8;
      ptr+=8;

      LOG_TRACE("SectionNumber: %d\n",   dwg->header.section[i].number)
      LOG_TRACE("SectionSize:   %x\n",   dwg->header.section[i].size)
      LOG_TRACE("SectionAddr:   %x\n", dwg->header.section[i].address)

      if (dwg->header.section[i].number<0)
        {
          dwg->header.section[i].parent  = *((int*)ptr);
          dwg->header.section[i].left  = *((int*)ptr+1);
          dwg->header.section[i].right  = *((int*)ptr+2);
          dwg->header.section[i].x00  = *((int*)ptr+3);
          bytes_remaining -= 16;
          ptr+=16;

          LOG_TRACE("Parent: %d\n",   (int)dwg->header.section[i].parent)
          LOG_TRACE("Left: %d\n",   (int)dwg->header.section[i].left)
          LOG_TRACE("Right: %d\n",   (int)dwg->header.section[i].right)
          LOG_TRACE("0x00: %d\n",   (int)dwg->header.section[i].x00)
        }

      dwg->header.num_sections++;
      i++;
    }
  free(decomp);
}

static Dwg_Section*
find_section(Dwg_Data *dwg, unsigned long int index)
{
  long unsigned int i;
  if (dwg->header.section == 0 || index == 0)
    return 0;
  for (i = 0; i < dwg->header.num_sections; ++i)
    {
      if ((unsigned long int)dwg->header.section[i].number == index)
        return (&dwg->header.section[i]);
    }
  return 0;
}

/* Read R2004 Section Info
 */
static void
read_R2004_section_info(Bit_Chain* dat, Dwg_Data *dwg,
                        unsigned long int comp_data_size,
                        unsigned long int decomp_data_size)
{
  char *decomp, *ptr;
  unsigned int i, j;
  int section_number;
  int data_size;
  int start_offset;
  int unknown;

  decomp = (char *)malloc(decomp_data_size * sizeof(char));
  if (decomp == 0)
    return;   // No memory

  decompress_R2004_section(dat, decomp, comp_data_size);

  memcpy(&dwg->header.num_descriptions, decomp, 4);
  dwg->header.section_info = (Dwg_Section_Info*)
    malloc(sizeof(Dwg_Section_Info) * dwg->header.num_descriptions);

  LOG_TRACE("\n#### 2004 Section Info fields ####\n")
  LOG_TRACE("NumDescriptions: %d\n", *((int*)decomp))
  LOG_TRACE("0x02:            %x\n", *((int*)decomp + 1))
  LOG_TRACE("0x00007400:      %x\n", *((int*)decomp + 2))
  LOG_TRACE("0x00:            %x\n", *((int*)decomp + 3))
  LOG_TRACE("Unknown:         %x\n", *((int*)decomp + 4))

  ptr = decomp + 20;
  for (i = 0; i < dwg->header.num_descriptions; ++i)
    {
      dwg->header.section_info[i].size            = *((int*)ptr);
      dwg->header.section_info[i].unknown1 	      = *((int*)ptr + 1);
      dwg->header.section_info[i].num_sections    = *((int*)ptr + 2);
      dwg->header.section_info[i].max_decomp_size = *((int*)ptr + 3);
      dwg->header.section_info[i].unknown2        = *((int*)ptr + 4);
      dwg->header.section_info[i].compressed      = *((int*)ptr + 5);
      dwg->header.section_info[i].type            = *((int*)ptr + 6);
      dwg->header.section_info[i].encrypted       = *((int*)ptr + 7);
      ptr += 32;
      memcpy(dwg->header.section_info[i].name, ptr, 64);
      ptr += 64;

      LOG_TRACE("\nSection Info description fields\n")
      LOG_TRACE("Size:                  %d\n",
           (int) dwg->header.section_info[i].size)
      LOG_TRACE("Unknown:               %d\n",
           (int) dwg->header.section_info[i].unknown1)
      LOG_TRACE("Number of sections:    %d\n",
           (int) dwg->header.section_info[i].num_sections)
      LOG_TRACE("Max decompressed size: %d\n",
           (int) dwg->header.section_info[i].max_decomp_size)
      LOG_TRACE("Unknown:               %d\n",
           (int) dwg->header.section_info[i].unknown2)
      LOG_TRACE("Compressed (0x02):     %x\n",
           (unsigned int) dwg->header.section_info[i].compressed)
      LOG_TRACE("Section Type:          %d\n",
           (int) dwg->header.section_info[i].type)
      LOG_TRACE("Encrypted:             %d\n",
           (int) dwg->header.section_info[i].encrypted)
      LOG_TRACE("SectionName:           %s\n\n",
            dwg->header.section_info[i].name)

      dwg->header.section_info[i].sections = (Dwg_Section**)
        malloc(dwg->header.section_info[i].num_sections * sizeof(Dwg_Section*));

      if (dwg->header.section_info[i].num_sections < 10000)
	{
	  LOG_INFO("section count %ld in area %d\n",dwg->header.section_info[i].num_sections,i)

	  for (j = 0; j < dwg->header.section_info[i].num_sections; j++)
	    {
	      section_number = *((int*)ptr);      // Index into SectionMap
	      data_size      = *((int*)ptr + 1);
	      start_offset   = *((int*)ptr + 2);
	      unknown        = *((int*)ptr + 3);  // high 32 bits of 64-bit start offset?
	      ptr += 16;

	      dwg->header.section_info[i].sections[j] = find_section(dwg, section_number);

	      LOG_TRACE("Section Number: %d\n", section_number)
	      LOG_TRACE("Data size:      %d\n", data_size)
	      LOG_TRACE("Start offset:   %x\n", start_offset)
	      LOG_TRACE("Unknown:        %d\n", unknown)
	    }
	}// sanity check
      else
	{
	  LOG_ERROR("section count %ld in area %d too high! skipping",dwg->header.section_info[i].num_sections,i)
	}
    }
  free(decomp);
}

/* Encrypted Section Header */
typedef union _encrypted_section_header
{
  unsigned long int long_data[8];
  unsigned char char_data[32];
  struct
  {
    unsigned long int tag;
    unsigned long int section_type;
    unsigned long int data_size;
    unsigned long int section_size;
    unsigned long int start_offset;
    unsigned long int unknown;
    unsigned long int checksum_1;
    unsigned long int checksum_2;
  } fields;
} encrypted_section_header;

static int
read_2004_compressed_section(Bit_Chain* dat, Dwg_Data *dwg,
                            Bit_Chain* sec_dat,
                            long unsigned int section_type)
{
  long unsigned int address, sec_mask;
  long unsigned int max_decomp_size;
  Dwg_Section_Info *info = 0;
  encrypted_section_header es;
  char *decomp;
  unsigned int i, j;

  for (i = 0; i < dwg->header.num_descriptions && info == 0; ++i)
    if (dwg->header.section_info[i].type == section_type)
      info = &dwg->header.section_info[i];

  if (info == 0)
    return 1;   // Failed to find section

  max_decomp_size = info->num_sections * info->max_decomp_size;

  decomp = (char *)malloc(max_decomp_size * sizeof(char));
  if (decomp == 0)
    return 2;   // No memory

  for (i = 0; i < info->num_sections; ++i)
    {
      address = info->sections[i]->address;
      dat->byte = address;

      for (j = 0; j < 0x20; j++)
        es.char_data[j] = bit_read_RC(dat);

      sec_mask = 0x4164536b ^ address;
      for (j = 0; j < 8; ++j)
        es.long_data[j] ^= sec_mask;

  LOG_INFO("\n=== Section (Class) ===\n")
  LOG_INFO("Section Tag (should be 0x4163043b): %x\n",
          (unsigned int) es.fields.tag)
  LOG_INFO("Section Type:     %x\n",
          (unsigned int) es.fields.section_type)
  LOG_INFO("Data size:   %x\n",
          (unsigned int) es.fields.data_size)   // this is the number of bytes that is read in decompress_R2004_section (+ 2bytes)
  LOG_INFO("Comp data size:     %x\n",
          (unsigned int) es.fields.section_size)
  LOG_INFO("StartOffset:      %x\n",
          (unsigned int) es.fields.start_offset)
  LOG_INFO("Unknown:          %x\n",
          (unsigned int) es.fields.unknown);
  LOG_INFO("Checksum1:        %x\n",
        (unsigned int) es.fields.checksum_1)
  LOG_INFO("Checksum2:        %x\n\n",
        (unsigned int) es.fields.checksum_2)

      decompress_R2004_section(dat, &decomp[i * info->max_decomp_size],
        es.fields.data_size);
    }

  sec_dat->bit     = 0;
  sec_dat->byte    = 0;
  sec_dat->chain   = (unsigned char *)decomp;
  sec_dat->size    = max_decomp_size;
  sec_dat->version = dat->version;
  sec_dat->from_version = dat->from_version;

  return 0;
}

/* R2004 Class Section
 */
static void
read_2004_section_classes(Bit_Chain* dat, Dwg_Data *dwg)
{
  unsigned long int size;
  unsigned long int max_num;
  unsigned long int num_objects, dwg_version, maint_version, unknown;
  char c;
  Bit_Chain sec_dat;

  if (read_2004_compressed_section(dat, dwg, &sec_dat, SECTION_CLASSES) != 0)
    return;

  if (bit_search_sentinel(&sec_dat, dwg_sentinel(DWG_SENTINEL_CLASS_BEGIN)))
    {
      size    = bit_read_RL(&sec_dat);  // size of class data area
      max_num = bit_read_BS(&sec_dat);  // Maximum class number
      c = bit_read_RC(&sec_dat);        // 0x00
      c = bit_read_RC(&sec_dat);        // 0x00
      c = bit_read_B(&sec_dat);         // 1

      dwg->dwg_ot_layout = 0;
      dwg->num_classes = 0;

      do
        {
          unsigned int idc;

          idc = dwg->num_classes;
          if (idc == 0)
            dwg->dwg_class = (Dwg_Class *) malloc(sizeof(Dwg_Class));
          else
            dwg->dwg_class = (Dwg_Class *) realloc(dwg->dwg_class, (idc + 1)
                * sizeof(Dwg_Class));

          dwg->dwg_class[idc].number        = bit_read_BS(&sec_dat);
          dwg->dwg_class[idc].version       = bit_read_BS(&sec_dat);
          dwg->dwg_class[idc].appname       = bit_read_TV(&sec_dat);
          dwg->dwg_class[idc].cppname       = bit_read_TV(&sec_dat);
          dwg->dwg_class[idc].dxfname       = bit_read_TV(&sec_dat);
          dwg->dwg_class[idc].wasazombie    = bit_read_B(&sec_dat);
          dwg->dwg_class[idc].item_class_id = bit_read_BS(&sec_dat);

          num_objects   = bit_read_BL(&sec_dat);  // DXF 91
          dwg_version   = bit_read_BS(&sec_dat);  // Dwg Version
          maint_version = bit_read_BS(&sec_dat);  // Maintenance release version.
          unknown       = bit_read_BL(&sec_dat);  // Unknown (normally 0L)
          unknown       = bit_read_BL(&sec_dat);  // Unknown (normally 0L)

          LOG_TRACE("-------------------\n")
          LOG_TRACE("Number:           %d\n", dwg->dwg_class[idc].number)
          LOG_TRACE("Version:          %x\n", dwg->dwg_class[idc].version)
          LOG_TRACE("Application name: %s\n", dwg->dwg_class[idc].appname)
          LOG_TRACE("C++ class name:   %s\n", dwg->dwg_class[idc].cppname)
          LOG_TRACE("DXF record name:  %s\n", dwg->dwg_class[idc].dxfname)

          if (strcmp((const char *)dwg->dwg_class[idc].dxfname, "LAYOUT") == 0)
            dwg->dwg_ot_layout = dwg->dwg_class[idc].number;

          dwg->num_classes++;

        } while (sec_dat.byte < (size - 1));
    }
    free(sec_dat.chain);
}

/* R2004 Header Section
 */
static void
read_2004_section_header(Bit_Chain* dat, Dwg_Data *dwg)
{
  Bit_Chain sec_dat;

  if (read_2004_compressed_section(dat, dwg, &sec_dat, SECTION_HEADER) != 0)
    return;

  if (bit_search_sentinel(&sec_dat, dwg_sentinel(DWG_SENTINEL_VARIABLE_BEGIN)))
    {
      unsigned long int size = bit_read_RL(&sec_dat);
      LOG_TRACE("Length: %lu\n", size);
      dwg_decode_header_variables(&sec_dat, dwg);
    }
  free(sec_dat.chain);
}

/* R2004 Handles Section
 */
static void
read_2004_section_handles(Bit_Chain* dat, Dwg_Data *dwg)
{
  unsigned int section_size = 0;
  unsigned char sgdc[2];
  long unsigned int duabyte;
  long unsigned int maplasta;
  Bit_Chain hdl_dat;
  Bit_Chain obj_dat;

  if (read_2004_compressed_section(dat, dwg, &obj_dat, SECTION_DBOBJECTS) != 0)
    return;

  if (read_2004_compressed_section(dat, dwg, &hdl_dat, SECTION_HANDLES) != 0)
    {
      free(obj_dat.chain);
      return;
    }

  maplasta = hdl_dat.byte + hdl_dat.size;
  dwg->num_objects = 0;

  do
    {
      long unsigned int last_offset;
      long unsigned int last_handle;
      long unsigned int previous_address = 0;

      duabyte = hdl_dat.byte;
      sgdc[0] = bit_read_RC(&hdl_dat);
      sgdc[1] = bit_read_RC(&hdl_dat);
      section_size = (sgdc[0] << 8) | sgdc[1];

      LOG_TRACE("section_size: %u\n", section_size);

      if (section_size > 2034)
        LOG_INFO("Error: Object-map section size greater than 2034!\n");

      last_handle = 0;
      last_offset = 0;
      while (hdl_dat.byte - duabyte < section_size)
        {
          long int pvztkt;
          long int pvzadr;

          previous_address = hdl_dat.byte;

          pvztkt = bit_read_MC(&hdl_dat);
          last_handle += pvztkt;

          pvzadr = bit_read_MC(&hdl_dat);
          last_offset += pvzadr;

          dwg_decode_add_object(dwg, &obj_dat, last_offset);
        }

      if (hdl_dat.byte == previous_address)
        break;
      hdl_dat.byte += 2; // CRC

      if (hdl_dat.byte >= maplasta)
        break;
    }
  while (section_size > 2);

  LOG_TRACE("\nNum objects: %lu\n", dwg->num_objects);

  free(hdl_dat.chain);
  free(obj_dat.chain);
}

static int
decode_R2004(Bit_Chain* dat, Dwg_Data * dwg)
{
  int j;
  /* Encrypted Data */
#if 0
  union
  {
    unsigned char encrypted_data[0x6c];
    struct
    {
      unsigned char file_ID_string[12];
      unsigned int header_offset;
      unsigned int header_size;
      unsigned int x04;
      unsigned int root_tree_node_gap;
      unsigned int lowermost_left_tree_node_gap;
      unsigned int lowermost_right_tree_node_gap;
      unsigned int unknown_long;
      unsigned int last_section_id;
      //FIXME: really is RLL
      unsigned int last_section_address;
      unsigned int x00_2;
      //FIXME: really is RLL
      unsigned int second_header_address;
      unsigned int x00_3;
      unsigned int gap_amount;
      unsigned int section_amount;
      unsigned int x20;
      unsigned int x80;
      unsigned int x40;
      unsigned int section_map_id;
      unsigned int section_map_address;
      unsigned int x00_4;
      unsigned int section_info_id;
      unsigned int section_array_size;
      unsigned int gap_array_size;
      unsigned int CRC;
    } fields;
  } _2004_header_data;
#endif

  /* System Section */
  typedef union _system_section
  {
    unsigned char data[0x14];
    struct
    {
      unsigned int section_type;   //0x4163043b
      unsigned int decomp_data_size;
      unsigned int comp_data_size;
      unsigned int compression_type;
      unsigned int checksum;
    } fields;
  } system_section;

  system_section ss;

  Dwg_Section *section;

  {
    struct Dwg_Header* _obj = &dwg->header;
    Dwg_Object *obj = NULL;
    int i;

    dat->byte = 0x06;

    #include "header.spec"

  }

  {
    Dwg_Object *obj = NULL;
    struct Dwg_R2004_Header* _obj = &dwg->r2004_header;
    const int size = sizeof(struct Dwg_R2004_Header);
    char encrypted_data[size];
    int rseed = 1;
    int i;

    dat->byte = 0x80;
    for (i = 0; i < size; i++)
      {
        rseed *= 0x343fd;
        rseed += 0x269ec3;
        encrypted_data[i] = bit_read_RC(dat) ^ (rseed >> 0x10);
      }
    LOG_TRACE("\n#### 2004 File Header Data fields ####\n");
    dat->byte = 0x80;
    if (dat->byte+0x80 >= dat->size - 1) {
      dat->size = dat->byte + 0x80;
      bit_chain_alloc(dat);
    }
    memcpy(&dat->chain[0x80], encrypted_data, size);

    #include "r2004_file_header.spec"

  }

  /*-------------------------------------------------------------------------
   * Section Map
   */
  dat->byte = dwg->r2004_header.section_map_address + 0x100;

  LOG_TRACE("\n\nRaw system section bytes:\n");
  for (j = 0; j < 0x14; j++)
    {
      ss.data[j] = bit_read_RC(dat);
      LOG_TRACE("%x ", ss.data[j])
    }

  LOG_TRACE("\n=== System Section (Section Map) ===\n")
  LOG_TRACE("Section Type (should be 0x4163043b): %x\n",
          (unsigned int) ss.fields.section_type)
  LOG_TRACE("DecompDataSize: %x\n",
          (unsigned int) ss.fields.decomp_data_size)
  LOG_TRACE("CompDataSize: %x\n",
          (unsigned int) ss.fields.comp_data_size)
  LOG_TRACE("Compression Type: %x\n",
          (unsigned int) ss.fields.compression_type)
  LOG_TRACE("Checksum: %x\n\n", (unsigned int) ss.fields.checksum)

  read_R2004_section_map(dat, dwg,
      ss.fields.comp_data_size, ss.fields.decomp_data_size);

  if (dwg->header.section == 0)
    {
      LOG_ERROR("Failed to read R2004 Section Map.")
      return -1;
    }

  /*-------------------------------------------------------------------------
   * Section Info
   */
  section = find_section(dwg, dwg->r2004_header.section_info_id);

  if (section != 0)
    {
      int i;
      dat->byte = section->address;
      LOG_TRACE("\nRaw system section bytes:\n")
      for (i = 0; i < 0x14; i++)
        {
          ss.data[i] = bit_read_RC(dat);
          LOG_TRACE("%x ", ss.data[i])
        }

      LOG_TRACE("\n=== System Section (Section Info) ===\n")
      LOG_TRACE("Section Type (should be 0x4163043b): %x\n",
              (unsigned int) ss.fields.section_type)
      LOG_TRACE("DecompDataSize: %x\n",
              (unsigned int) ss.fields.decomp_data_size)
      LOG_TRACE("CompDataSize: %x\n",
              (unsigned int) ss.fields.comp_data_size)
      LOG_TRACE("Compression Type: %x\n",
              (unsigned int) ss.fields.compression_type)
      LOG_TRACE("Checksum: %x\n\n", (unsigned int) ss.fields.checksum)

       read_R2004_section_info(dat, dwg,
         ss.fields.comp_data_size, ss.fields.decomp_data_size);
    }

  read_2004_section_classes(dat, dwg);
  read_2004_section_header(dat, dwg);
  read_2004_section_handles(dat, dwg);

  /* Clean up */
  if (dwg->header.section_info != 0)
    {
      unsigned u;
      for (u = 0; u < dwg->header.num_descriptions; ++u)
        if (dwg->header.section_info[u].sections != 0)
          free(dwg->header.section_info[u].sections);

      free(dwg->header.section_info);
      dwg->header.num_descriptions = 0;
    }

  resolve_objectref_vector(dwg);

  return 0;
}

static int
decode_R2007(Bit_Chain* dat, Dwg_Data * dwg)
{
  {
    int i;
    struct Dwg_Header *_obj = &dwg->header;
    Dwg_Object *obj = NULL;

    dat->byte = 0x06;
    #include "header.spec"
  }

  read_r2007_meta_data(dat, dwg);

  LOG_TRACE("\n")

  /////////////////////////////////////////
  //	incomplete implementation!
  /////////////////////////////////////////
  LOG_INFO(
      "Decoding of DWG version R2007+ objectrefs is not fully implemented yet.\n"
      "We are going to try\n")
  return resolve_objectref_vector(dwg);
}

/*--------------------------------------------------------------------------------
 * Private functions
 */

static int
dwg_decode_entity(Bit_Chain * dat, Dwg_Object_Entity * ent)
{
  unsigned int i;
  unsigned int size;
  int error = 2;

  SINCE(R_2000)
    {
      ent->bitsize = bit_read_RL(dat);
    }

  error = bit_read_H(dat, &(ent->object->handle));
  if (error)
    {
      LOG_ERROR(
          "dwg_decode_entity:\tError in object handle! Current Bit_Chain address: 0x%0x",
          (unsigned int) dat->byte)
      ent->bitsize = 0;
      ent->extended_size = 0;
      ent->picture_exists = 0;
      ent->num_handles = 0;
      return 0;
    }

  ent->extended_size = 0;
  while ((size = bit_read_BS(dat)))
    {
      LOG_TRACE("EED size: %lu\n", (long unsigned int)size)
      if (size > 10210)
        {
          LOG_ERROR(
              "dwg_decode_entity: Absurd! Extended object data size: %lu. Object: %lu (handle)",
              (long unsigned int) size, ent->object->handle.value)
          ent->bitsize = 0;
          ent->extended_size = 0;
          ent->picture_exists = 0;
          ent->num_handles = 0;
          //XXX
          return -1;
          //break;
        }
      if (ent->extended_size == 0)
        {
          ent->extended = (char *)malloc(size);
          ent->extended_size = size;
        }
      else
        {
          ent->extended_size += size;
          ent->extended = (char *)realloc(ent->extended, ent->extended_size);
        }
      error = bit_read_H(dat, &ent->extended_handle);
      if (error)
        LOG_ERROR("Error reading extended handle!");
      for (i = ent->extended_size - size; i < ent->extended_size; i++)
        ent->extended[i] = bit_read_RC(dat);
    }
  ent->picture_exists = bit_read_B(dat);
  if (ent->picture_exists)
    {
      LOG_TRACE("picture_exists: 1\n")
      ent->picture_size = bit_read_RL(dat);
      LOG_TRACE("picture_size: " FORMAT_RS " \n", ent->picture_size)
      if (ent->picture_size < 210210)
        {
          ent->picture = (char *)malloc(ent->picture_size);
          for (i = 0; i < ent->picture_size; i++)
            ent->picture[i] = bit_read_RC(dat);
        }
      else
        {
          LOG_ERROR(
              "dwg_decode_entity:  Absurd! Picture-size: %u kB. Object: %lu (handle)",
              ent->picture_size / 1000, ent->object->handle.value)
          bit_advance_position(dat, -(4 * 8 + 1));
        }
    }

  VERSIONS(R_13,R_14)
    {
      ent->bitsize = bit_read_RL(dat);
    }

  ent->entity_mode = bit_read_BB(dat);
  ent->num_reactors = bit_read_BL(dat);

  SINCE(R_2004)
    {
      ent->xdic_missing_flag = bit_read_B(dat);
    }

  VERSIONS(R_13,R_14)
    {
      ent->isbylayerlt = bit_read_B(dat);
    }

  ent->nolinks = bit_read_B(dat);

  SINCE(R_2004)
    {
      BITCODE_B color_mode = 0;
      unsigned int flags;

      if (ent->nolinks == 0)
        {
          color_mode = bit_read_B(dat);

          if (color_mode == 1)
            {
              ent->color.index = bit_read_RC(dat);  // color index
              ent->color.rgb = 0L;
            }
          else
            {
              flags = bit_read_RS(dat);

              if (flags & 0x8000)
                {
                  unsigned char c1, c2, c3, c4;
                  char *name;

                  c1 = bit_read_RC(dat);  // rgb color
                  c2 = bit_read_RC(dat);
                  c3 = bit_read_RC(dat);
                  c4 = bit_read_RC(dat);
                  name = bit_read_TV(dat);
                  ent->color.index = 0;
                  ent->color.rgb   = c1 << 24 | c2 << 16 | c3 << 8 | c4;
                  strcpy(ent->color.name, name);
                }

              /*if (flags & 0x4000)
                flags = flags;   // has AcDbColor reference (handle)
              */
              if (flags & 0x2000)
                {
                  ent->color.transparency_type = bit_read_BL(dat);
                }
            }
        }
      else
        {
          char color = bit_read_B(dat);
          ent->color.index = color;
        }
    }
  OTHER_VERSIONS
    bit_read_CMC(dat, &ent->color);

  ent->linetype_scale = bit_read_BD(dat);

  SINCE(R_2000)
    {
      ent->linetype_flags = bit_read_BB(dat);
      ent->plotstyle_flags = bit_read_BB(dat);
    }

  SINCE(R_2007)
    {
      ent->material_flags = bit_read_BB(dat);
      ent->shadow_flags = bit_read_RC(dat);
    }

  ent->invisible = bit_read_BS(dat);

  SINCE(R_2000)
    {
      ent->lineweight = bit_read_RC(dat);
    }

  return 0;
}

static int
dwg_decode_object(Bit_Chain * dat, Dwg_Object_Object * obj)
{
  unsigned int i;
  unsigned int size;
  int error = 2;

  SINCE(R_2000)
    {
      obj->bitsize = bit_read_RL(dat);
      LOG_INFO("Object bitsize: " FORMAT_RL "\n", obj->bitsize);
    }

  error = bit_read_H(dat, &obj->object->handle);
  if (error)
    {
      LOG_ERROR(
          "\tError in object handle! Bit_Chain current address: 0x%0x",
          (unsigned int) dat->byte)
      obj->bitsize = 0;
      obj->extended_size = 0;
      obj->num_handles = 0;
      return -1;
    }
  obj->extended_size = 0;
  while ((size = bit_read_BS(dat)))
    {
      if (size > 10210)
        {
          LOG_ERROR(
              "dwg_decode_object: Absurd! Extended object data size: %lu. Object: %lu (handle)",
              (long unsigned int) size, obj->object->handle.value)
          obj->bitsize = 0;
          obj->extended_size = 0;
          obj->num_handles = 0;
          return 0;
        }
      if (obj->extended_size == 0)
        {
          obj->extended = (unsigned char *)malloc(size);
          obj->extended_size = size;
        }
      else
        {
          obj->extended_size += size;
          obj->extended = (unsigned char *)realloc(obj->extended, obj->extended_size);
        }
      error = bit_read_H(dat, &obj->extended_handle);
      if (error)
        LOG_ERROR("Error reading extended handle!")
      for (i = obj->extended_size - size; i < obj->extended_size; i++)
        obj->extended[i] = bit_read_RC(dat);
    }

  VERSIONS(R_13,R_14)
    {
      obj->bitsize = bit_read_RL(dat);
    }

  obj->num_reactors = bit_read_BL(dat);

  SINCE(R_2004)
    {
      obj->xdic_missing_flag = bit_read_B(dat);
    }

  return 0;
}

/**
 * Find a pointer to an object given it's id (handle)
 */
static Dwg_Object *
dwg_resolve_handle(Dwg_Data* dwg, long unsigned int absref)
{
  //FIXME find a faster algorithm
  long unsigned int i;
  for (i = 0; i < dwg->num_objects; i++)
    {
      if (dwg->object[i].handle.value == absref)
        {
          return &dwg->object[i];
        }
    }
  LOG_WARN("Object not found: %lu in %ld objects", absref, dwg->num_objects)
  return NULL;
}

static Dwg_Object_Ref *
dwg_decode_handleref(Bit_Chain * dat, Dwg_Object * obj, Dwg_Data* dwg)
{
  // Welcome to the house of evil code!
  Dwg_Object_Ref* ref = (Dwg_Object_Ref *) malloc(sizeof(Dwg_Object_Ref));

  if (bit_read_H(dat, &ref->handleref))
    {
      if (obj)
        {
          LOG_ERROR(
            "Could not read handleref in object whose handle is: %d.%d.%lu",
            obj->handle.code, obj->handle.size, obj->handle.value)
        }
      else
        {
          LOG_ERROR("Could not read handleref in the header variables section")
        }
      free(ref);
      return 0;
    }

  //if the handle size is 0, it is probably a null handle. It
  //shouldn't be placed in the object ref vector
  if (ref->handleref.size)
    {
      //Reserve memory space for object references
      if (dwg->num_object_refs == 0)
        dwg->object_ref = (Dwg_Object_Ref **) malloc(REFS_PER_REALLOC * sizeof(Dwg_Object_Ref*));
      else
        if (dwg->num_object_refs % REFS_PER_REALLOC == 0)
          {
            dwg->object_ref = (Dwg_Object_Ref **) realloc(dwg->object_ref,
                                (dwg->num_object_refs + REFS_PER_REALLOC) * sizeof(Dwg_Object_Ref*));
          }
      dwg->object_ref[dwg->num_object_refs++] = ref;
    }
  else
    {
      ref->obj = 0;
      ref->absolute_ref = 0;
      return ref;
    }
  //we receive a null obj when we are reading
  // handles in the header variables section
  if (!obj)
    {
      ref->absolute_ref = ref->handleref.value;
      ref->obj = 0;
      return ref;
    }
  /*
   * sometimes the code indicates the type of ownership
   * in other cases the handle is stored as an offset from some other handle
   * how is it determined?
   */
 switch(ref->handleref.code) //that's right: don't bother the code on the spec.
    {
    case 0x06: //what if 6 means HARD_OWNER?
      ref->absolute_ref = (obj->handle.value + 1);
      break;
    case 0x08:
      ref->absolute_ref = (obj->handle.value - 1);
      break;
    case 0x0A:
      ref->absolute_ref = (obj->handle.value + ref->handleref.value);
      break;
    case 0x0C:
      ref->absolute_ref = (obj->handle.value - ref->handleref.value);
      break;
    default: //0x02, 0x03, 0x04, 0x05 or none
      ref->absolute_ref = ref->handleref.value;
      break;
    }
  return ref;
}

static Dwg_Object_Ref *
dwg_decode_handleref_with_code(Bit_Chain * dat, Dwg_Object * obj, Dwg_Data* dwg, unsigned int code)
{
  Dwg_Object_Ref * ref;
  ref = dwg_decode_handleref(dat, obj, dwg);

  if (!ref)
    {
      LOG_ERROR("dwg_decode_handleref_with_code: ref is a null pointer");
      return 0;
    }

  if (ref->absolute_ref == 0 && ref->handleref.code != code)
    {
      LOG_WARN("Expected a CODE %d handle, got a %d", code, ref->handleref.code)
      //TODO: At the moment we are tolerating wrong codes in handles.
      // in the future we might want to get strict and return 0 here so that code will crash
      // whenever it reaches the first handle parsing error. This might make debugging easier.
      //return 0;
    }
  return ref;
}

static void
dwg_decode_header_variables(Bit_Chain* dat, Dwg_Data * dwg)
{
  Dwg_Header_Variables* _obj = &dwg->header_vars;
  Dwg_Object* obj = NULL;

  #include "header_variables.spec"
}

static void
dwg_decode_common_entity_handle_data(Bit_Chain * dat, Dwg_Object * obj)
{

  Dwg_Object_Entity *ent;
  Dwg_Data *dwg = obj->parent;
  long unsigned int vcount;
  Dwg_Object_Entity *_obj;

  ent = obj->tio.entity;
  _obj = ent;

  #include "common_entity_handle_data.spec"

}

static enum RES_BUF_VALUE_TYPE
get_base_value_type(short gc)
{
  if (gc >= 300)
    {
      if (gc >= 440)
        {
          if (gc >= 1000)  // 1000-1071
            {
              if (gc == 1004) return VT_BINARY;
              if (gc <= 1009) return VT_STRING;
              if (gc <= 1059) return VT_REAL;
              if (gc <= 1070) return VT_INT16;
              if (gc == 1071) return VT_INT32;
            }
          else            // 440-999
            {
              if (gc <= 459) return VT_INT32;
              if (gc <= 469) return VT_REAL;
              if (gc <= 479) return VT_STRING;
              if (gc <= 998) return VT_INVALID;
              if (gc == 999) return VT_STRING;
            }
        }
      else // <440
        {
          if (gc >= 390)  // 390-439
            {
              if (gc <= 399) return VT_HANDLE;
              if (gc <= 409) return VT_INT16;
              if (gc <= 419) return VT_STRING;
              if (gc <= 429) return VT_INT32;
              if (gc <= 439) return VT_STRING;
            }
          else            // 330-389
            {
              if (gc <= 309) return VT_STRING;
              if (gc <= 319) return VT_BINARY;
              if (gc <= 329) return VT_HANDLE;
              if (gc <= 369) return VT_OBJECTID;
              if (gc <= 389) return VT_INT16;
            }
        }
    }
  else if (gc >= 105)
    {
      if (gc >= 210)      // 210-299
        {
          if (gc <= 239) return VT_REAL;
          if (gc <= 269) return VT_INVALID;
          if (gc <= 279) return VT_INT16;
          if (gc <= 289) return VT_INT8;
          if (gc <= 299) return VT_BOOL;
        }
      else               // 105-209
        {
          if (gc == 105) return VT_HANDLE;
          if (gc <= 109) return VT_INVALID;
          if (gc <= 149) return VT_REAL;
          if (gc <= 169) return VT_INVALID;
          if (gc <= 179) return VT_INT16;
          if (gc <= 209) return VT_INVALID;
        }
    }
  else  // <105
    {
      if (gc >= 38)     // 38-102
        {
          if (gc <= 59)  return VT_REAL;
          if (gc <= 79)  return VT_INT16;
          if (gc <= 99)  return VT_INT32;
          if (gc <= 101) return VT_STRING;
          if (gc == 102) return VT_STRING;
        }
      else              // 0-37
        {
          if (gc <= 0)   return VT_INVALID;
          if (gc <= 4)   return VT_STRING;
          if (gc == 5)   return VT_HANDLE;
          if (gc <= 9)   return VT_STRING;
          if (gc <= 37)  return VT_POINT3D;
        }
    }
  return VT_INVALID;
}

static Dwg_Resbuf*
dwg_decode_xdata(Bit_Chain * dat, int size)
{
  Dwg_Resbuf *rbuf, *root=0, *curr=0;
  unsigned char codepage;
  long unsigned int end_address;
  int i, length;

  static int cnt = 0;
  cnt++;

  end_address = dat->byte + (unsigned long int)size;

  while (dat->byte < end_address)
    {
      rbuf = (Dwg_Resbuf *) malloc(sizeof(Dwg_Resbuf));
      rbuf->next = 0;
      rbuf->type = bit_read_RS(dat);

      switch (get_base_value_type(rbuf->type))
        {
        case VT_STRING:
          length   = bit_read_RS(dat);
          codepage = bit_read_RC(dat);
          if (length > 0)
            {
              rbuf->value.str = (char *)malloc((length + 1) * sizeof(char));
              for (i = 0; i < length; i++)
                rbuf->value.str[i] = bit_read_RC(dat);
              rbuf->value.str[i] = '\0';
            }
          break;
        case VT_REAL:
          rbuf->value.dbl = bit_read_RD(dat);
          break;
        case VT_BOOL:
        case VT_INT8:
          rbuf->value.i8 = bit_read_RC(dat);
          break;
        case VT_INT16:
          rbuf->value.i16 = bit_read_RS(dat);
          break;
        case VT_INT32:
          rbuf->value.i32 = bit_read_RL(dat);
          break;
        case VT_POINT3D:
          rbuf->value.pt[0] = bit_read_RD(dat);
          rbuf->value.pt[1] = bit_read_RD(dat);
          rbuf->value.pt[2] = bit_read_RD(dat);
          break;
        case VT_BINARY:
          rbuf->value.chunk.size = bit_read_RC(dat);
          if (rbuf->value.chunk.size > 0)
            {
              rbuf->value.chunk.data = (char *)malloc(rbuf->value.chunk.size * sizeof(char));
              for (i = 0; i < rbuf->value.chunk.size; i++)
                rbuf->value.chunk.data[i] = bit_read_RC(dat);
            }
          break;
        case VT_HANDLE:
        case VT_OBJECTID:
          for (i = 0; i < 8; i++)
             rbuf->value.hdl[i] = bit_read_RC(dat);
          break;
        case VT_INVALID:
        default:
          LOG_ERROR("Invalid group code in xdata: %d", rbuf->type)
          free(rbuf);
          dat->byte = end_address;
          return root;
          break;
        }

      if (curr == 0)
        curr = root = rbuf;
      else
        {
          curr->next = rbuf;
          curr = rbuf;
        }
    }
    return root;
}

/* OBJECTS *******************************************************************/

#include<dwg.spec>


/*--------------------------------------------------------------------------------
 * Private functions which depend on the preceding
 */

/* returns 1 if object could be decoded and 0 otherwise
 */
static int
dwg_decode_variable_type(Dwg_Data * dwg, Bit_Chain * dat, Dwg_Object* obj)
{
  int i;

  if ((obj->type - 500) > dwg->num_classes)
    return 0;

  i = obj->type - 500;

  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "ACDBDICTIONARYWDFLT"))
    {
      dwg_decode_DICTIONARYWDLFT(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "DICTIONARYVAR"))
    {
      dwg_decode_DICTIONARYVAR(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "HATCH"))
    {
      dwg_decode_HATCH(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "IDBUFFER"))
    {
      dwg_decode_IDBUFFER(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "IMAGE"))
    {
      dwg_decode_IMAGE(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "IMAGEDEF"))
    {
      dwg_decode_IMAGEDEF(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "IMAGEDEF_REACTOR"))
    {
      dwg_decode_IMAGEDEF_REACTOR(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "LAYER_INDEX"))
    {
      dwg_decode_LAYER_INDEX(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "LAYOUT"))
    {
      dwg_decode_LAYOUT(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "LWPLINE"))
    {
      dwg_decode_LWPLINE(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "OLE2FRAME"))
    {
      dwg_decode_OLE2FRAME(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "ACDBPLACEHOLDER"))
    {
      dwg_decode_PLACEHOLDER(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "PROXY"))
    {
      dwg_decode_PROXY(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "RASTERVARIABLES"))
    {
      dwg_decode_RASTERVARIABLES(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "SORTENTSTABLE"))
    {
      dwg_decode_SORTENTSTABLE(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "SPATIAL_FILTER"))
    {
      dwg_decode_SPATIAL_FILTER(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "SPATIAL_INDEX"))
    {
      dwg_decode_SPATIAL_INDEX(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "TABLE"))
    {
      dwg_decode_TABLE(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "VBA_PROJECT"))
    {
      LOG_ERROR("Unhandled Object VBA_PROJECT. Has its own section");
      //dwg_decode_VBA_PROJECT(dat, obj);
      return 0;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "XRECORD"))
    {
      dwg_decode_XRECORD(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "WIPEOUTVARIABLE"))
    {
      // TODO
      LOG_WARN("Unhandled Object/Class %s\n", dwg->dwg_class[i].dxfname);
      //dwg_decode_WIPEOUTVARIABLE(dat, obj);
      return 0;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "WIPEOUT"))
    {
      // TODO Entity? obj->bitsize: 4783
      //LOG_WARN("Unhandled Object/Class %s\n", dwg->dwg_class[i].dxfname);
      dwg_decode_WIPEOUT(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "CELLSTYLEMAP"))
    {
      // TODO
      LOG_WARN("Unhandled Object/Class %s\n", dwg->dwg_class[i].dxfname);
      dwg_decode_CELLSTYLEMAP(dat, obj);
      return 0;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "VISUALSTYLE"))
    {
      // TODO
      LOG_WARN("Unhandled Object/Class %s\n", dwg->dwg_class[i].dxfname);
      //dwg_decode_VISUALSTYLE(dat, obj);
      return 0;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "AcDbField")) //??
    {
      // TODO
      LOG_WARN("Untested Object/Class %s\n", dwg->dwg_class[i].dxfname);
      dwg_decode_FIELD(dat, obj);
      return 1;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "DIMASSOC"))
    {
      LOG_WARN("Unhandled Object/Class %s\n", dwg->dwg_class[i].dxfname);
//TODO:      dwg_decode_DIMASSOC(dat, obj);
      return 0;
    }
  if (!strcmp((const char *)dwg->dwg_class[i].dxfname, "MATERIAL"))
    {
      LOG_WARN("Unhandled Object/Class %s\n", dwg->dwg_class[i].dxfname);
//TODO:      dwg_decode_MATERIAL(dat, obj);
      return 0;
    }

  LOG_WARN("Unknown Object/Class %s\n", dwg->dwg_class[i].dxfname);
  /* TABLE, CELLSTYLEMAP, DBCOLOR, DICTIONARYVAR, DICTIONARYWDFLT,
     FIELD, GROUP, HATCH, IDBUFFER, IMAGE, IMAGEDEF, IMAGEDEFREACTOR,
     LAYER_INDEX, LAYOUT, LWPLINE, MATERIAL, MLEADER, MLEADERSTYLE,
     OLE2FRAME, PLACEHOLDER, PLOTSETTINGS, RASTERVARIABLES, SCALE,
     SORTENTSTABLE, SPATIAL_FILTER, SPATIAL_INDEX, TABLEGEOMETRY,
     TABLESTYLE, VBA_PROJECT, VISUALSTYLE, WIPEOUTVARIABLE, XRECORD
  */

  return 0;
}

static void
dwg_decode_add_object(Dwg_Data * dwg, Bit_Chain * dat,
    long unsigned int address)
{
  long unsigned int previous_address;
  long unsigned int object_address;
  unsigned char previous_bit;
  Dwg_Object *obj;

  /* Keep the previous address
   */
  previous_address = dat->byte;
  previous_bit = dat->bit;

  /* Use the indicated address for the object
   */
  dat->byte = address;
  dat->bit = 0;

  /*
   * Reserve memory space for objects
   */
  if (dwg->num_objects == 0)
    dwg->object = (Dwg_Object *) malloc(sizeof(Dwg_Object));
  else
    dwg->object = (Dwg_Object *) realloc(dwg->object, (dwg->num_objects + 1)
        * sizeof(Dwg_Object));

  if (loglevel)
      LOG_INFO("\n\n======================\nObject number: %lu",
          dwg->num_objects)

  obj = &dwg->object[dwg->num_objects];
  obj->index = dwg->num_objects;
  dwg->num_objects++;

  obj->handle.code = 0;
  obj->handle.size = 0;
  obj->handle.value = 0;

  obj->parent = dwg;
  obj->size = bit_read_MS(dat);
  object_address = dat->byte;
  ktl_lastaddress = dat->byte + obj->size; /* (calculate the bitsize) */
  obj->type = bit_read_BS(dat);

  LOG_INFO(" Type: %d\n", obj->type)

  /* Check the type of the object
   */
  switch (obj->type)
    {
    case DWG_TYPE_TEXT:
      dwg_decode_TEXT(dat, obj);
      break;
    case DWG_TYPE_ATTRIB:
      dwg_decode_ATTRIB(dat, obj);
      break;
    case DWG_TYPE_ATTDEF:
      dwg_decode_ATTDEF(dat, obj);
      break;
    case DWG_TYPE_BLOCK:
      dwg_decode_BLOCK(dat, obj);
      break;
    case DWG_TYPE_ENDBLK:
      dwg_decode_ENDBLK(dat, obj);
      break;
    case DWG_TYPE_SEQEND:
      dwg_decode_SEQEND(dat, obj);
      break;
    case DWG_TYPE_INSERT:
      dwg_decode_INSERT(dat, obj);
      break;
    case DWG_TYPE_MINSERT:
      dwg_decode_MINSERT(dat, obj);
      break;
    case DWG_TYPE_VERTEX_2D:
      dwg_decode_VERTEX_2D(dat, obj);
      break;
    case DWG_TYPE_VERTEX_3D:
      dwg_decode_VERTEX_3D(dat, obj);
      break;
    case DWG_TYPE_VERTEX_MESH:
      dwg_decode_VERTEX_MESH(dat, obj);
      break;
    case DWG_TYPE_VERTEX_PFACE:
      dwg_decode_VERTEX_PFACE(dat, obj);
      break;
    case DWG_TYPE_VERTEX_PFACE_FACE:
      dwg_decode_VERTEX_PFACE_FACE(dat, obj);
      break;
    case DWG_TYPE_POLYLINE_2D:
      dwg_decode_POLYLINE_2D(dat, obj);
      break;
    case DWG_TYPE_POLYLINE_3D:
      dwg_decode_POLYLINE_3D(dat, obj);
      break;
    case DWG_TYPE_ARC:
      dwg_decode_ARC(dat, obj);
      break;
    case DWG_TYPE_CIRCLE:
      dwg_decode_CIRCLE(dat, obj);
      break;
    case DWG_TYPE_LINE:
      dwg_decode_LINE(dat, obj);
      break;
    case DWG_TYPE_DIMENSION_ORDINATE:
      dwg_decode_DIMENSION_ORDINATE(dat, obj);
      break;
    case DWG_TYPE_DIMENSION_LINEAR:
      dwg_decode_DIMENSION_LINEAR(dat, obj);
      break;
    case DWG_TYPE_DIMENSION_ALIGNED:
      dwg_decode_DIMENSION_ALIGNED(dat, obj);
      break;
    case DWG_TYPE_DIMENSION_ANG3PT:
      dwg_decode_DIMENSION_ANG3PT(dat, obj);
      break;
    case DWG_TYPE_DIMENSION_ANG2LN:
      dwg_decode_DIMENSION_ANG2LN(dat, obj);
      break;
    case DWG_TYPE_DIMENSION_RADIUS:
      dwg_decode_DIMENSION_RADIUS(dat, obj);
      break;
    case DWG_TYPE_DIMENSION_DIAMETER:
      dwg_decode_DIMENSION_DIAMETER(dat, obj);
      break;
    case DWG_TYPE_POINT:
      dwg_decode_POINT(dat, obj);
      break;
    case DWG_TYPE__3DFACE:
      dwg_decode__3DFACE(dat, obj);
      break;
    case DWG_TYPE_POLYLINE_PFACE:
      dwg_decode_POLYLINE_PFACE(dat, obj);
      break;
    case DWG_TYPE_POLYLINE_MESH:
      dwg_decode_POLYLINE_MESH(dat, obj);
      break;
    case DWG_TYPE_SOLID:
      dwg_decode_SOLID(dat, obj);
      break;
    case DWG_TYPE_TRACE:
      dwg_decode_TRACE(dat, obj);
      break;
    case DWG_TYPE_SHAPE:
      dwg_decode_SHAPE(dat, obj);
      break;
    case DWG_TYPE_VIEWPORT:
      dwg_decode_VIEWPORT(dat, obj);
      break;
    case DWG_TYPE_ELLIPSE:
      dwg_decode_ELLIPSE(dat, obj);
      break;
    case DWG_TYPE_SPLINE:
      dwg_decode_SPLINE(dat, obj);
      break;
    case DWG_TYPE_REGION:
      dwg_decode_REGION(dat, obj);
      break;
    case DWG_TYPE_3DSOLID:
      dwg_decode__3DSOLID(dat, obj);
      break;
    case DWG_TYPE_BODY:
      dwg_decode_BODY(dat, obj);
      break;
    case DWG_TYPE_RAY:
      dwg_decode_RAY(dat, obj);
      break;
    case DWG_TYPE_XLINE:
      dwg_decode_XLINE(dat, obj);
      break;
    case DWG_TYPE_DICTIONARY:
      dwg_decode_DICTIONARY(dat, obj);
      break;
    case DWG_TYPE_MTEXT:
      dwg_decode_MTEXT(dat, obj);
      break;
    case DWG_TYPE_LEADER:
      dwg_decode_LEADER(dat, obj);
      break;
    case DWG_TYPE_TOLERANCE:
      dwg_decode_TOLERANCE(dat, obj);
      break;
    case DWG_TYPE_MLINE:
      dwg_decode_MLINE(dat, obj);
      break;
    case DWG_TYPE_BLOCK_CONTROL:
      dwg_decode_BLOCK_CONTROL(dat, obj);
      break;
    case DWG_TYPE_BLOCK_HEADER:
      dwg_decode_BLOCK_HEADER(dat, obj);
      break;
    case DWG_TYPE_LAYER_CONTROL:
      //set LAYER_CONTROL object - helps keep track of layers
      obj->parent->layer_control = obj;
      dwg_decode_LAYER_CONTROL(dat, obj);
      break;
    case DWG_TYPE_LAYER:
      dwg_decode_LAYER(dat, obj);
      break;
    case DWG_TYPE_SHAPEFILE_CONTROL:
      dwg_decode_SHAPEFILE_CONTROL(dat, obj);
      break;
    case DWG_TYPE_SHAPEFILE:
      dwg_decode_SHAPEFILE(dat, obj);
      break;
    case DWG_TYPE_LTYPE_CONTROL:
      dwg_decode_LTYPE_CONTROL(dat, obj);
      break;
    case DWG_TYPE_LTYPE:
      dwg_decode_LTYPE(dat, obj);
      break;
    case DWG_TYPE_VIEW_CONTROL:
      dwg_decode_VIEW_CONTROL(dat, obj);
      break;
    case DWG_TYPE_VIEW:
      dwg_decode_VIEW(dat, obj);
      break;
    case DWG_TYPE_UCS_CONTROL:
      dwg_decode_UCS_CONTROL(dat, obj);
      break;
    case DWG_TYPE_UCS:
      dwg_decode_UCS(dat, obj);
      break;
    case DWG_TYPE_VPORT_CONTROL:
      dwg_decode_VPORT_CONTROL(dat, obj);
      break;
    case DWG_TYPE_VPORT:
      dwg_decode_VPORT(dat, obj);
      break;
    case DWG_TYPE_APPID_CONTROL:
      dwg_decode_APPID_CONTROL(dat, obj);
      break;
    case DWG_TYPE_APPID:
      dwg_decode_APPID(dat, obj);
      break;
    case DWG_TYPE_DIMSTYLE_CONTROL:
      dwg_decode_DIMSTYLE_CONTROL(dat, obj);
      break;
    case DWG_TYPE_DIMSTYLE:
      dwg_decode_DIMSTYLE(dat, obj);
      break;
    case DWG_TYPE_VP_ENT_HDR_CONTROL:
      dwg_decode_VP_ENT_HDR_CONTROL(dat, obj);
      break;
    case DWG_TYPE_VP_ENT_HDR:
      dwg_decode_VP_ENT_HDR(dat, obj);
      break;
    case DWG_TYPE_GROUP:
      dwg_decode_GROUP(dat, obj);
      break;
    case DWG_TYPE_MLINESTYLE:
      dwg_decode_MLINESTYLE(dat, obj);
      break;
    case DWG_TYPE_OLE2FRAME:
      dwg_decode_OLE2FRAME(dat, obj);
      break;
    case DWG_TYPE_DUMMY:
      dwg_decode_DUMMY(dat, obj);
      break;
    case DWG_TYPE_LONG_TRANSACTION:
      dwg_decode_LONG_TRANSACTION(dat, obj);
      break;
    case DWG_TYPE_LWPLINE:
      dwg_decode_LWPLINE(dat, obj);
      break;
    case DWG_TYPE_HATCH:
      dwg_decode_HATCH(dat, obj);
      break;
    case DWG_TYPE_XRECORD:
      dwg_decode_XRECORD(dat, obj);
      break;
    case DWG_TYPE_PLACEHOLDER:
      dwg_decode_PLACEHOLDER(dat, obj);
      break;
    case DWG_TYPE_PROXY_ENTITY:
      dwg_decode_PROXY_ENTITY(dat, obj);
      break;
    case DWG_TYPE_OLEFRAME:
      dwg_decode_OLEFRAME(dat, obj);
      break;
    case DWG_TYPE_VBA_PROJECT:
      LOG_ERROR("Unhandled Object VBA_PROJECT. Has its own section");
      //dwg_decode_VBA_PROJECT(dat, obj);
      break;
    case DWG_TYPE_LAYOUT:
      dwg_decode_LAYOUT(dat, obj);
      break;
    default:
      if (obj->type == obj->parent->dwg_ot_layout)
        {
          dwg_decode_LAYOUT(dat, obj);
        }
      /* > 500:
         TABLE, DICTIONARYWDLFT, IDBUFFER, IMAGE, IMAGEDEF, IMAGEDEFREACTOR,
         LAYER_INDEX, OLE2FRAME, PROXY, RASTERVARIABLES, SORTENTSTABLE, SPATIAL_FILTER,
         SPATIAL_INDEX, WIPEOUTVARIABLES
      */
      else if (!dwg_decode_variable_type(dwg, dat, obj))
        {
          LOG_INFO("Object UNKNOWN:\n")

#if 0
          dwg_decode_object(dat, obj->tio.object);
#else
          SINCE(R_2000)
            {
              BITCODE_RL bitsize = bit_read_RL(dat);
              LOG_INFO("Object bitsize: " FORMAT_RL "\n", bitsize);
              obj->bitsize = bitsize;
            }

          if (!bit_read_H(dat, &obj->handle))
            {
              LOG_INFO("Object handle: %d.%d.%lu\n",
                       obj->handle.code, obj->handle.size, obj->handle.value)
            }
#endif
          obj->supertype = DWG_SUPERTYPE_UNKNOWN;
          /* neither object nor entity, at least we don't know yet */
          obj->tio.unknown = (unsigned char*)malloc(obj->size);
          memcpy(obj->tio.unknown, &dat->chain[object_address], obj->size);
        }
    }

  /*
   if (obj->supertype != DWG_SUPERTYPE_UNKNOWN)
     {
       fprintf (stderr, " Begin address:\t%10lu\n", address);
       fprintf (stderr, " Last address:\t%10lu\tSize: %10lu\n", dat->byte, obj->size);
       fprintf (stderr, "End address:\t%10lu (calculated)\n", address + 2 + obj->size);
     }
   */

  /* Register the previous addresses for return
   */
  dat->byte = previous_address;
  dat->bit = previous_bit;
}

#undef IS_DECODER
