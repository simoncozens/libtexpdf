/* This is dvipdfmx, an eXtended version of dvipdfm by Mark A. Wicks.

    Copyright (C) 2008-2015 by Jin-Hwan Cho, Matthias Franz, and Shunsaku Hirata,
    the dvipdfmx project team.
    
    Copyright (C) 1998, 1999 by Mark A. Wicks <mwicks@kettering.edu>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
*/

/*
 * TODO: Many things...
 *  {begin,end}_{bead,article}, box stack, name tree (not limited to dests)...
 */

#include "libtexpdf.h"
#if HAVE_LIBPNG
#include "pngimage.h"
#endif

#include <time.h>

#define PDFDOC_PAGES_ALLOC_SIZE   128u
#define PDFDOC_ARTICLE_ALLOC_SIZE 16
#define PDFDOC_BEAD_ALLOC_SIZE    16

/* XXX Need to eliminate statics if this is going to be reentrant! */
static int verbose = 0;
static char *thumb_basename = NULL;

static char * my_name = "libtexpdf";

void
texpdf_doc_enable_manual_thumbnails (pdf_doc* p)
{
#if HAVE_LIBPNG
  p->manual_thumb_enabled = 1;
#else
  WARN("Manual thumbnail is not supported without the libpng library.");
#endif
}

static pdf_obj *
read_thumbnail (pdf_doc *p, const char *thumb_filename) 
{
  pdf_obj *image_ref;
  int      xobj_id;
  FILE    *fp;

  fp = MFOPEN(thumb_filename, FOPEN_RBIN_MODE);
  if (!fp) {
    WARN("Could not open thumbnail file \"%s\"", thumb_filename);
    return NULL;
  }
  if (!texpdf_check_for_png(fp) && !texpdf_check_for_jpeg(fp)) {
    WARN("Thumbnail \"%s\" not a png/jpeg file!", thumb_filename);
    MFCLOSE(fp);
    return NULL;
  }
  MFCLOSE(fp);

  xobj_id = texpdf_ximage_findresource(p, thumb_filename, 0, NULL);
  if (xobj_id < 0) {
    WARN("Could not read thumbnail file \"%s\".", thumb_filename);
    image_ref = NULL;
  } else {
    image_ref = texpdf_ximage_get_reference(xobj_id);
  }

  return image_ref;
}

void
texpdf_doc_set_verbose (void)
{
  verbose++;
  pdf_font_set_verbose();
  texpdf_color_set_verbose();
  texpdf_ximage_set_verbose();
}

typedef struct pdf_form
{
  char       *ident;

  pdf_tmatrix matrix;
  pdf_rect    cropbox;

  pdf_obj    *resources;
  pdf_obj    *contents;
} pdf_form;

struct form_list_node
{
  int      q_depth;
  pdf_form form;

  struct form_list_node *prev;
};

#define USE_MY_MEDIABOX (1 << 0)


struct name_dict
{
  const char  *category;
  struct ht_table *data;
};

static void
pdf_doc_init_catalog (pdf_doc *p)
{
  p->root.viewerpref = NULL;
  p->root.pagelabels = NULL;
  p->root.pages      = NULL;
  p->root.names      = NULL;
  p->root.threads    = NULL;
  
  p->root.dict = texpdf_new_dict();
  texpdf_set_root(p->root.dict);

  return;
}

static void
pdf_doc_close_catalog (pdf_doc *p)
{
  pdf_obj *tmp;

  if (p->root.viewerpref) {
    tmp = texpdf_lookup_dict(p->root.dict, "ViewerPreferences");
    if (!tmp) {
      texpdf_add_dict(p->root.dict,
                   texpdf_new_name("ViewerPreferences"),
                   texpdf_ref_obj (p->root.viewerpref));
    } else if (PDF_OBJ_DICTTYPE(tmp)) {
      texpdf_merge_dict(p->root.viewerpref, tmp);
      texpdf_add_dict(p->root.dict,
                   texpdf_new_name("ViewerPreferences"),
                   texpdf_ref_obj (p->root.viewerpref));
    } else { /* Maybe reference */
      /* What should I do? */
      WARN("Could not modify ViewerPreferences.");
    }
    texpdf_release_obj(p->root.viewerpref);
    p->root.viewerpref = NULL;
  }

  if (p->root.pagelabels) {
    tmp = texpdf_lookup_dict(p->root.dict, "PageLabels");
    if (!tmp) {
      tmp = texpdf_new_dict();
      texpdf_add_dict(tmp, texpdf_new_name("Nums"),  texpdf_link_obj(p->root.pagelabels));
      texpdf_add_dict(p->root.dict,
                   texpdf_new_name("PageLabels"), texpdf_ref_obj(tmp));
      texpdf_release_obj(tmp);
    } else { /* Maybe reference */
      /* What should I do? */
      WARN("Could not modify PageLabels.");
    }
    texpdf_release_obj(p->root.pagelabels);
    p->root.pagelabels = NULL;
  }

  texpdf_add_dict(p->root.dict,
               texpdf_new_name("Type"), texpdf_new_name("Catalog"));
  texpdf_release_obj(p->root.dict);
  p->root.dict = NULL;

  return;
}

/*
 * Pages are starting at 1.
 * The page count does not increase until the page is finished.
 */
#define LASTPAGE(p)  (&(p->pages.entries[p->pages.num_entries]))
#define FIRSTPAGE(p) (&(p->pages.entries[0]))
#define PAGECOUNT(p) (p->pages.num_entries)
#define MAXPAGES(p)  (p->pages.max_entries)

static void
doc_resize_page_entries (pdf_doc *p, long size)
{
  if (size > MAXPAGES(p)) {
    long i;

    p->pages.entries = RENEW(p->pages.entries, size, struct pdf_page);
    for (i = p->pages.max_entries; i < size; i++) {
      p->pages.entries[i].page_obj   = NULL;
      p->pages.entries[i].page_ref   = NULL;
      p->pages.entries[i].flags      = 0;
      p->pages.entries[i].resources  = NULL;
      p->pages.entries[i].background = NULL;
      p->pages.entries[i].contents   = NULL;
      p->pages.entries[i].content_refs[0] = NULL; /* global bop */
      p->pages.entries[i].content_refs[1] = NULL; /* background */
      p->pages.entries[i].content_refs[2] = NULL; /* page body  */
      p->pages.entries[i].content_refs[3] = NULL; /* global eop */
      p->pages.entries[i].annots    = NULL;
      p->pages.entries[i].beads     = NULL;
    }
    p->pages.max_entries = size;
  }

  return;
}

static pdf_page *
doc_get_page_entry (pdf_doc *p, unsigned long page_no)
{
  pdf_page *page;

  if (page_no > 65535ul) {
    ERROR("Page number %ul too large!", page_no);
  } else if (page_no == 0) {
    ERROR("Invalid Page number %ul.", page_no);
  }

  if (page_no > MAXPAGES(p)) {
    doc_resize_page_entries(p, page_no + PDFDOC_PAGES_ALLOC_SIZE);
  }

  page = &(p->pages.entries[page_no - 1]);

  return page;
}

static void pdf_doc_init_page_tree  (pdf_doc *p, double media_width, double media_height);
static void pdf_doc_close_page_tree (pdf_doc *p);

static void pdf_doc_init_names  (pdf_doc *p, int check_gotos);
static void pdf_doc_close_names (pdf_doc *p);

static void pdf_doc_add_goto (pdf_doc *p, pdf_obj *annot_dict);

static void pdf_doc_init_docinfo  (pdf_doc *p);
static void pdf_doc_close_docinfo (pdf_doc *p);

static void pdf_doc_init_articles    (pdf_doc *p);
static void pdf_doc_close_articles   (pdf_doc *p);
static void pdf_doc_init_bookmarks   (pdf_doc *p, int bm_open_depth);
static void pdf_doc_close_bookmarks  (pdf_doc *p);

void
texpdf_doc_set_bop_content (pdf_doc *p, const char *content, unsigned length)
{
  ASSERT(p);

  if (p->pages.bop) {
    texpdf_release_obj(p->pages.bop);
    p->pages.bop = NULL;
  }

  if (length > 0) {
    p->pages.bop = texpdf_new_stream(STREAM_COMPRESS);
    texpdf_add_stream(p->pages.bop, content, length);
  } else {
    p->pages.bop = NULL;
  }

  return;
}

void
texpdf_doc_set_eop_content (pdf_doc *p, const char *content, unsigned length)
{
  if (p->pages.eop) {
    texpdf_release_obj(p->pages.eop);
    p->pages.eop = NULL;
  }

  if (length > 0) {
    p->pages.eop = texpdf_new_stream(STREAM_COMPRESS);
    texpdf_add_stream(p->pages.eop, content, length);
  } else {
    p->pages.eop = NULL;
  }

  return;
}

#ifndef HAVE_TM_GMTOFF
#ifndef HAVE_TIMEZONE

/* auxiliary function to compute timezone offset on
   systems that do not support the tm_gmtoff in struct tm,
   or have a timezone variable.  Such as i386-solaris.  */

static long
compute_timezone_offset()
{
  const time_t now = time(NULL);
  struct tm tm;
  struct tm local;
  time_t gmtoff;

  localtime_r(&now, &local);
  gmtime_r(&now, &tm);
  return (mktime(&local) - mktime(&tm));
}

#endif /* HAVE_TIMEZONE */
#endif /* HAVE_TM_GMTOFF */

/*
 * Docinfo
 */
static long
asn_date (char *date_string)
{
  long        tz_offset;
  time_t      current_time;
  struct tm  *bd_time;

  time(&current_time);
  bd_time = localtime(&current_time);

#ifdef HAVE_TM_GMTOFF
  tz_offset = bd_time->tm_gmtoff;
#else
#  ifdef HAVE_TIMEZONE
  tz_offset = -timezone;
#  else
  tz_offset = compute_timezone_offset();
#  endif /* HAVE_TIMEZONE */
#endif /* HAVE_TM_GMTOFF */

  sprintf(date_string, "D:%04d%02d%02d%02d%02d%02d%c%02ld'%02ld'",
	  bd_time->tm_year + 1900, bd_time->tm_mon + 1, bd_time->tm_mday,
	  bd_time->tm_hour, bd_time->tm_min, bd_time->tm_sec,
	  (tz_offset > 0) ? '+' : '-', labs(tz_offset) / 3600,
                                      (labs(tz_offset) / 60) % 60);

  return strlen(date_string);
}

static void
pdf_doc_init_docinfo (pdf_doc *p)
{
  p->info = texpdf_new_dict();
  texpdf_set_info(p->info);

  return;
}

static void
pdf_doc_close_docinfo (pdf_doc *p)
{
  pdf_obj *docinfo = p->info;

  /*
   * Excerpt from PDF Reference 4th ed., sec. 10.2.1.
   *
   * Any entry whose value is not known should be omitted from the dictionary,
   * rather than included with an empty string as its value.
   *
   * ....
   *
   * Note: Although viewer applications can store custom metadata in the document
   * information dictionary, it is inappropriate to store private content or
   * structural information there; such information should be stored in the
   * document catalog instead (see Section 3.6.1,  Document Catalog ).
   */
  const char *keys[] = {
    "Title", "Author", "Subject", "Keywords", "Creator", "Producer",
    "CreationDate", "ModDate", /* Date */
    NULL
  };
  pdf_obj *value;
  int      i;

  for (i = 0; keys[i] != NULL; i++) {
    value = texpdf_lookup_dict(docinfo, keys[i]);
    if (value) {
      if (!PDF_OBJ_STRINGTYPE(value)) {
        WARN("\"%s\" in DocInfo dictionary not string type.", keys[i]);
        texpdf_remove_dict(docinfo, keys[i]);
        WARN("\"%s\" removed from DocInfo.", keys[i]);
      } else if (texpdf_string_length(value) == 0) {
        /* The hyperref package often uses emtpy strings. */
        texpdf_remove_dict(docinfo, keys[i]);
      }
    }
  }

  if (!texpdf_lookup_dict(docinfo, "Producer")) {
    char *banner;

    banner = NEW(strlen(my_name)+strlen(PACKAGE_VERSION)+4, char);
    sprintf(banner, "%s (%s)", my_name, PACKAGE_VERSION);
    texpdf_add_dict(docinfo,
                 texpdf_new_name("Producer"),
                 texpdf_new_string(banner, strlen(banner)));
    RELEASE(banner);
  }
  
  if (!texpdf_lookup_dict(docinfo, "CreationDate")) {
    char now[32];

    asn_date(now);
    texpdf_add_dict(docinfo, 
                 texpdf_new_name ("CreationDate"),
                 texpdf_new_string(now, strlen(now)));
  }

  texpdf_release_obj(docinfo);
  p->info = NULL;

  return;
}

static pdf_obj *
texpdf_doc_get_page_resources (pdf_doc *p, const char *category)
{
  pdf_obj  *resources;
  pdf_page *currentpage;
  pdf_obj  *res_dict;

  if (!p || !category) {
    return NULL;
  }

  if (p->pending_forms) {
    if (p->pending_forms->form.resources) {
      res_dict = p->pending_forms->form.resources;
    } else {
      res_dict = p->pending_forms->form.resources = texpdf_new_dict();
    }
  } else {
    currentpage = LASTPAGE(p);
    if (currentpage->resources) {
      res_dict = currentpage->resources;
    } else {
      res_dict = currentpage->resources = texpdf_new_dict();
    }
  }
  resources = texpdf_lookup_dict(res_dict, category);
  if (!resources) {
    resources = texpdf_new_dict();
    texpdf_add_dict(res_dict, texpdf_new_name(category), resources);
  }

  return resources;
}

void
texpdf_doc_add_page_resource (pdf_doc *p, const char *category,
                           const char *resource_name, pdf_obj *resource_ref)
{
  pdf_obj *resources;
  pdf_obj *duplicate;

  if (!PDF_OBJ_INDIRECTTYPE(resource_ref)) {
    WARN("Passed non indirect reference...");
    resource_ref = texpdf_ref_obj(resource_ref); /* leak */
  }
  resources = texpdf_doc_get_page_resources(p, category);
  duplicate = texpdf_lookup_dict(resources, resource_name);
  if (duplicate && pdf_compare_reference(duplicate, resource_ref)) {
    WARN("Conflicting page resource found (page: %ld, category: %s, name: %s).",
         texpdf_doc_current_page_number(p), category, resource_name);
    WARN("Ignoring...");
    texpdf_release_obj(resource_ref);
  } else {
    texpdf_add_dict(resources, texpdf_new_name(resource_name), resource_ref);
  }

  return;
}

static void
doc_flush_page (pdf_doc *p, pdf_page *page, pdf_obj *parent_ref)
{
  pdf_obj *contents_array;
  int      count;

  texpdf_add_dict(page->page_obj,
               texpdf_new_name("Type"), texpdf_new_name("Page"));
  texpdf_add_dict(page->page_obj,
               texpdf_new_name("Parent"), parent_ref);

  /*
   * Clipping area specified by CropBox is affected by MediaBox which
   * might be inherit from parent node. If MediaBox of the root node
   * does not have enough size to cover all page's imaging area, using
   * CropBox here gives incorrect result.
   */
  if (page->flags & USE_MY_MEDIABOX) {
    pdf_obj *mediabox;

    mediabox = texpdf_new_array();
    texpdf_add_array(mediabox,
                  texpdf_new_number(ROUND(page->cropbox.llx, 0.01)));
    texpdf_add_array(mediabox,
                  texpdf_new_number(ROUND(page->cropbox.lly, 0.01)));
    texpdf_add_array(mediabox,
                  texpdf_new_number(ROUND(page->cropbox.urx, 0.01)));
    texpdf_add_array(mediabox,
                  texpdf_new_number(ROUND(page->cropbox.ury, 0.01)));
    texpdf_add_dict(page->page_obj, texpdf_new_name("MediaBox"),  mediabox);
  }

  count = 0;
  contents_array = texpdf_new_array();
  if (page->content_refs[0]) { /* global bop */
    texpdf_add_array(contents_array, page->content_refs[0]);
    count++;
  } else if (p->pages.bop &&
             pdf_stream_length(p->pages.bop) > 0) {
    texpdf_add_array(contents_array, texpdf_ref_obj(p->pages.bop));
    count++;
  }
  if (page->content_refs[1]) { /* background */
    texpdf_add_array(contents_array, page->content_refs[1]);
    count++;
  }
  if (page->content_refs[2]) { /* page body */
    texpdf_add_array(contents_array, page->content_refs[2]);
    count++;
  }
  if (page->content_refs[3]) { /* global eop */
    texpdf_add_array(contents_array, page->content_refs[3]);
    count++;
  } else if (p->pages.eop &&
             pdf_stream_length(p->pages.eop) > 0) {
    texpdf_add_array(contents_array, texpdf_ref_obj(p->pages.eop));
    count++;
  }

  if (count == 0) {
    WARN("Page with empty content found!!!");
  }
  page->content_refs[0] = NULL;
  page->content_refs[1] = NULL;
  page->content_refs[2] = NULL;
  page->content_refs[3] = NULL;

  texpdf_add_dict(page->page_obj,
               texpdf_new_name("Contents"), contents_array);


  if (page->annots) {
    texpdf_add_dict(page->page_obj,
                 texpdf_new_name("Annots"), texpdf_ref_obj(page->annots));
    texpdf_release_obj(page->annots);
  }
  if (page->beads) {
    texpdf_add_dict(page->page_obj,
                 texpdf_new_name("B"), texpdf_ref_obj(page->beads));
    texpdf_release_obj(page->beads);
  }
  texpdf_release_obj(page->page_obj);
  texpdf_release_obj(page->page_ref);

  page->page_obj = NULL;
  page->page_ref = NULL;
  page->annots   = NULL;
  page->beads    = NULL;

  return;
}

/* B-tree? */
#define PAGE_CLUSTER 4
static pdf_obj *
build_page_tree (pdf_doc  *p,
                 pdf_page *firstpage, long num_pages,
                 pdf_obj  *parent_ref)
{
  pdf_obj *self, *self_ref, *kids;
  long     i;

  self = texpdf_new_dict();
  /*
   * This is a slight kludge which allow the subtree dictionary
   * generated by this routine to be merged with the real
   * page_tree dictionary, while keeping the indirect object
   * references right.
   */
  self_ref = parent_ref ? texpdf_ref_obj(self) : texpdf_ref_obj(p->root.pages);

  texpdf_add_dict(self, texpdf_new_name("Type"),  texpdf_new_name("Pages"));
  texpdf_add_dict(self, texpdf_new_name("Count"), texpdf_new_number((double) num_pages));

  if (parent_ref != NULL)
    texpdf_add_dict(self, texpdf_new_name("Parent"), parent_ref);

  kids = texpdf_new_array();
  if (num_pages > 0 && num_pages <= PAGE_CLUSTER) {
    for (i = 0; i < num_pages; i++) {
      pdf_page *page;

      page = firstpage + i;
      if (!page->page_ref)
        page->page_ref = texpdf_ref_obj(page->page_obj);
      texpdf_add_array (kids, texpdf_link_obj(page->page_ref));
      doc_flush_page(p, page, texpdf_link_obj(self_ref));
    }
  } else if (num_pages > 0) {
    for (i = 0; i < PAGE_CLUSTER; i++) {
      long start, end;

      start = (i*num_pages)/PAGE_CLUSTER;
      end   = ((i+1)*num_pages)/PAGE_CLUSTER;
      if (end - start > 1) {
        pdf_obj *subtree;

        subtree = build_page_tree(p, firstpage + start, end - start,
                                  texpdf_link_obj(self_ref));
        texpdf_add_array(kids, texpdf_ref_obj(subtree));
        texpdf_release_obj(subtree);
      } else {
        pdf_page *page;

        page = firstpage + start;
        if (!page->page_ref)
          page->page_ref = texpdf_ref_obj(page->page_obj);
        texpdf_add_array (kids, texpdf_link_obj(page->page_ref));
        doc_flush_page(p, page, texpdf_link_obj(self_ref));
      }
    }
  }
  texpdf_add_dict(self, texpdf_new_name("Kids"), kids);
  texpdf_release_obj(self_ref);

  return self;
}

static void
pdf_doc_init_page_tree (pdf_doc *p, double media_width, double media_height)
{
  /*
   * Create empty page tree.
   * The docroot.pages is kept open until the document is closed.
   * This allows the user to write to pages if he so choses.
   */
  p->root.pages = texpdf_new_dict();

  p->pages.num_entries = 0;
  p->pages.max_entries = 0;
  p->pages.entries     = NULL;

  p->pages.bop = NULL;
  p->pages.eop = NULL;

  p->pages.mediabox.llx = 0.0;
  p->pages.mediabox.lly = 0.0;
  p->pages.mediabox.urx = media_width;
  p->pages.mediabox.ury = media_height;

  return;
}

static void
pdf_doc_close_page_tree (pdf_doc *p)
{
  pdf_obj *page_tree_root;
  pdf_obj *mediabox;
  long     page_no;

  /*
   * Do consistency check on forward references to pages.
   */
  for (page_no = PAGECOUNT(p) + 1; page_no <= MAXPAGES(p); page_no++) {
    pdf_page  *page;

    page = doc_get_page_entry(p, page_no);
    if (page->page_obj) {
      WARN("Nonexistent page #%ld refered.", page_no);
      texpdf_release_obj(page->page_ref);
      page->page_ref = NULL;
    }
    if (page->page_obj) {
      WARN("Entry for a nonexistent page #%ld created.", page_no);
      texpdf_release_obj(page->page_obj);
      page->page_obj = NULL;
    }
    if (page->annots) {
      WARN("Annotation attached to a nonexistent page #%ld.", page_no);
      texpdf_release_obj(page->annots);
      page->annots = NULL;
    }
    if (page->beads) {
      WARN("Article beads attached to a nonexistent page #%ld.", page_no);
      texpdf_release_obj(page->beads);
      page->beads = NULL;
    }
    if (page->resources) {
      texpdf_release_obj(page->resources);
      page->resources = NULL;
    }
  }

  /*
   * Connect page tree to root node.
   */
  page_tree_root = build_page_tree(p, FIRSTPAGE(p), PAGECOUNT(p), NULL);
  texpdf_merge_dict (p->root.pages, page_tree_root);
  texpdf_release_obj(page_tree_root);

  /* They must be after build_page_tree() */
  if (p->pages.bop) {
    texpdf_add_stream (p->pages.bop, "\n", 1);
    texpdf_release_obj(p->pages.bop);
    p->pages.bop = NULL;
  }
  if (p->pages.eop) {
    texpdf_add_stream (p->pages.eop, "\n", 1);
    texpdf_release_obj(p->pages.eop);
    p->pages.eop = NULL;
  }

  /* Create media box at root node and let the other pages inherit it. */
  mediabox = texpdf_new_array();
  texpdf_add_array(mediabox, texpdf_new_number(ROUND(p->pages.mediabox.llx, 0.01)));
  texpdf_add_array(mediabox, texpdf_new_number(ROUND(p->pages.mediabox.lly, 0.01)));
  texpdf_add_array(mediabox, texpdf_new_number(ROUND(p->pages.mediabox.urx, 0.01)));
  texpdf_add_array(mediabox, texpdf_new_number(ROUND(p->pages.mediabox.ury, 0.01)));
  texpdf_add_dict(p->root.pages, texpdf_new_name("MediaBox"), mediabox);

  texpdf_add_dict(p->root.dict,
               texpdf_new_name("Pages"),
               texpdf_ref_obj (p->root.pages));
  texpdf_release_obj(p->root.pages);
  p->root.pages  = NULL;

  RELEASE(p->pages.entries);
  p->pages.entries     = NULL;
  p->pages.num_entries = 0;
  p->pages.max_entries = 0;

  return;
}

/*
 * From PDFReference15_v6.pdf (p.119 and p.834)
 *
 * MediaBox rectangle (Required; inheritable)
 *
 * The media box defines the boundaries of the physical medium on which the
 * page is to be printed. It may include any extended area surrounding the
 * finished page for bleed, printing marks, or other such purposes. It may
 * also include areas close to the edges of the medium that cannot be marked
 * because of physical limitations of the output device. Content falling
 * outside this boundary can safely be discarded without affecting the
 * meaning of the PDF file.
 *
 * CropBox rectangle (Optional; inheritable)
 *
 * The crop box defines the region to which the contents of the page are to be
 * clipped (cropped) when displayed or printed. Unlike the other boxes, the
 * crop box has no defined meaning in terms of physical page geometry or
 * intended use; it merely imposes clipping on the page contents. However,
 * in the absence of additional information (such as imposition instructions
 * specified in a JDF or PJTF job ticket), the crop box will determine how
 * the page's contents are to be positioned on the output medium. The default
 * value is the page's media box. 
 *
 * BleedBox rectangle (Optional; PDF 1.3)
 *
 * The bleed box (PDF 1.3) defines the region to which the contents of the
 * page should be clipped when output in a production environment. This may
 * include any extra "bleed area" needed to accommodate the physical
 * limitations of cutting, folding, and trimming equipment. The actual printed
 * page may include printing marks that fall outside the bleed box.
 * The default value is the page's crop box. 
 *
 * TrimBox rectangle (Optional; PDF 1.3)
 *
 * The trim box (PDF 1.3) defines the intended dimensions of the finished page
 * after trimming. It may be smaller than the media box, to allow for
 * production-related content such as printing instructions, cut marks, or
 * color bars. The default value is the page's crop box. 
 *
 * ArtBox rectangle (Optional; PDF 1.3)
 *
 * The art box (PDF 1.3) defines the extent of the page's meaningful content
 * (including potential white space) as intended by the page's creator.
 * The default value is the page's crop box.
 *
 * Rotate integer (Optional; inheritable)
 *
 * The number of degrees by which the page should be rotated clockwise when
 * displayed or printed. The value must be a multiple of 90. Default value: 0.
 */

pdf_obj *
texpdf_doc_get_page (pdf_file *pf, long page_no, long *count_p,
		  pdf_rect *bbox, pdf_obj **resources_p) {
  pdf_obj *page_tree = NULL;
  pdf_obj *resources = NULL, *box = NULL, *rotate = NULL;
  pdf_obj *catalog;

  catalog = pdf_file_get_catalog(pf);

  page_tree = pdf_deref_obj(texpdf_lookup_dict(catalog, "Pages"));

  if (!PDF_OBJ_DICTTYPE(page_tree))
    goto error;

  {
    long count;
    pdf_obj *tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "Count"));
    if (!PDF_OBJ_NUMBERTYPE(tmp)) {
      if (tmp)
	texpdf_release_obj(tmp);
      goto error;
    }
    count = texpdf_number_value(tmp);
    texpdf_release_obj(tmp);
    if (count_p)
      *count_p = count;
    if (page_no <= 0 || page_no > count) {
	WARN("Page %ld does not exist.", page_no);
	goto error_silent;
      }
  }

  /*
   * Seek correct page. Get MediaBox, CropBox and Resources.
   * (Note that these entries can be inherited.)
   */
  {
    pdf_obj *media_box = NULL, *crop_box = NULL, *kids, *tmp;
    int depth = PDF_OBJ_MAX_DEPTH;
    long page_idx = page_no-1, kids_length = 1, i = 0;

    while (--depth && i != kids_length) {
      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "MediaBox")))) {
	if (media_box)
	  texpdf_release_obj(media_box);
	media_box = tmp;
      }

      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "CropBox")))) {
	if (crop_box)
	  texpdf_release_obj(crop_box);
	crop_box = tmp;
      }

      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "Rotate")))) {
	if (rotate)
	  texpdf_release_obj(rotate);
	rotate = tmp;
      }

      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "Resources")))) {
	if (resources)
	  texpdf_release_obj(resources);
	resources = tmp;
      }

      kids = pdf_deref_obj(texpdf_lookup_dict(page_tree, "Kids"));
      if (!kids)
	break;
      else if (!PDF_OBJ_ARRAYTYPE(kids)) {
	texpdf_release_obj(kids);
	goto error;
      }
      kids_length = texpdf_array_length(kids);

      for (i = 0; i < kids_length; i++) {
	long count;

	texpdf_release_obj(page_tree);
	page_tree = pdf_deref_obj(texpdf_get_array(kids, i));
	if (!PDF_OBJ_DICTTYPE(page_tree))
	  goto error;

	tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "Count"));
	if (PDF_OBJ_NUMBERTYPE(tmp)) {
	  /* Pages object */
	  count = texpdf_number_value(tmp);
	  texpdf_release_obj(tmp);
	} else if (!tmp)
	  /* Page object */
	  count = 1;
	else {
	  texpdf_release_obj(tmp);
	  goto error;
	}

	if (page_idx < count)
	  break;

	page_idx -= count;
      }
      
      texpdf_release_obj(kids);
    }

    if (!depth || kids_length == i) {
      if (media_box)
	texpdf_release_obj(media_box);
     if (crop_box)
	texpdf_release_obj(crop_box);
      goto error;
    }

    if (crop_box)
      box = crop_box;
    else
      if (!(box = pdf_deref_obj(texpdf_lookup_dict(page_tree, "ArtBox"))) &&
	  !(box = pdf_deref_obj(texpdf_lookup_dict(page_tree, "TrimBox"))) &&
	  !(box = pdf_deref_obj(texpdf_lookup_dict(page_tree, "BleedBox"))) &&
	  media_box) {
	  box = media_box;
	  media_box = NULL;
      }
    if (media_box)
      texpdf_release_obj(media_box);
  }

  if (!PDF_OBJ_ARRAYTYPE(box) || texpdf_array_length(box) != 4 ||
      !PDF_OBJ_DICTTYPE(resources))
    goto error;

  if (PDF_OBJ_NUMBERTYPE(rotate)) {
    if (texpdf_number_value(rotate))
      WARN("<< /Rotate %d >> found. (Not supported yet)", 
	   (int) texpdf_number_value(rotate));
    texpdf_release_obj(rotate);
    rotate = NULL;
  } else if (rotate)
    goto error;

  {
    int i;

    for (i = 4; i--; ) {
      double x;
      pdf_obj *tmp = pdf_deref_obj(texpdf_get_array(box, i));
      if (!PDF_OBJ_NUMBERTYPE(tmp)) {
	texpdf_release_obj(tmp);
	goto error;
      }
      x = texpdf_number_value(tmp);
      switch (i) {
      case 0: bbox->llx = x; break;
      case 1: bbox->lly = x; break;
      case 2: bbox->urx = x; break;
      case 3: bbox->ury = x; break;
      }
      texpdf_release_obj(tmp);
    }
  }

  texpdf_release_obj(box);

  if (resources_p)
    *resources_p = resources;
  else if (resources)
    texpdf_release_obj(resources);

  return page_tree;

 error:
  WARN("Cannot parse document. Broken PDF file?");
 error_silent:
  if (box)
    texpdf_release_obj(box);
  if (rotate)
    texpdf_release_obj(rotate);
  if (resources)
    texpdf_release_obj(resources);
  if (page_tree)
    texpdf_release_obj(page_tree);

  return NULL;
}

#ifndef BOOKMARKS_OPEN_DEFAULT
#define BOOKMARKS_OPEN_DEFAULT 0
#endif

static int clean_bookmarks (pdf_olitem *item);
static int flush_bookmarks (pdf_olitem *item,
                            pdf_obj *parent_ref,
                            pdf_obj *parent_dict);

static void
pdf_doc_init_bookmarks (pdf_doc *p, int bm_open_depth)
{
  pdf_olitem *item;

#define MAX_OUTLINE_DEPTH 256u
  p->opt.outline_open_depth =
    ((bm_open_depth >= 0) ?
     bm_open_depth : MAX_OUTLINE_DEPTH - bm_open_depth);

  p->outlines.current_depth = 1;

  item = NEW(1, pdf_olitem);
  item->dict    = NULL;
  item->next    = NULL;
  item->first   = NULL;
  item->parent  = NULL;
  item->is_open = 1;

  p->outlines.current = item;
  p->outlines.first   = item;

  return;
}

static int
clean_bookmarks (pdf_olitem *item)
{
  pdf_olitem *next;

  while (item) {
    next = item->next;
    if (item->dict)
      texpdf_release_obj(item->dict);
    if (item->first)
      clean_bookmarks(item->first);
    RELEASE(item);
    
    item = next;
  }

  return 0;
}

static int
flush_bookmarks (pdf_olitem *node,
                 pdf_obj *parent_ref, pdf_obj *parent_dict)
{
  int         retval;
  int         count;
  pdf_olitem *item;
  pdf_obj    *this_ref, *prev_ref, *next_ref;

  ASSERT(node->dict);

  this_ref = texpdf_ref_obj(node->dict);
  texpdf_add_dict(parent_dict,
               texpdf_new_name("First"), texpdf_link_obj(this_ref));

  retval = 0;
  for (item = node, prev_ref = NULL;
       item && item->dict; item = item->next) {
    if (item->first && item->first->dict) {
      count = flush_bookmarks(item->first, this_ref, item->dict);
      if (item->is_open) {
        texpdf_add_dict(item->dict,
                     texpdf_new_name("Count"),
                     texpdf_new_number(count));
        retval += count;
      } else {
        texpdf_add_dict(item->dict,
                     texpdf_new_name("Count"),
                     texpdf_new_number(-count));
      }
    }
    texpdf_add_dict(item->dict,
                 texpdf_new_name("Parent"),
                 texpdf_link_obj(parent_ref));
    if (prev_ref) {
      texpdf_add_dict(item->dict,
                   texpdf_new_name("Prev"),
                   prev_ref);
    }
    if (item->next && item->next->dict) {
      next_ref = texpdf_ref_obj(item->next->dict);
      texpdf_add_dict(item->dict,
                   texpdf_new_name("Next"),
                   texpdf_link_obj(next_ref));
    } else {
      next_ref = NULL;
    }

    texpdf_release_obj(item->dict);
    item->dict = NULL;

    prev_ref = this_ref;
    this_ref = next_ref;
    retval++;    
  }

  texpdf_add_dict(parent_dict,
               texpdf_new_name("Last"),
               texpdf_link_obj(prev_ref));

  texpdf_release_obj(prev_ref);
  texpdf_release_obj(node->dict);
  node->dict = NULL;

  return retval;
}
  
int
texpdf_doc_bookmarks_up (pdf_doc *p)
{
  pdf_olitem *parent, *item;

  item = p->outlines.current;
  if (!item || !item->parent) {
    WARN("Can't go up above the bookmark root node!");
    return -1;
  }
  parent = item->parent;
  item   = parent->next;
  if (!parent->next) {
    parent->next  = item = NEW(1, pdf_olitem);
    item->dict    = NULL;
    item->first   = NULL;
    item->next    = NULL;
    item->is_open = 0;
    item->parent  = parent->parent;
  }
  p->outlines.current = item;
  p->outlines.current_depth--;

  return 0;
}

int
texpdf_doc_bookmarks_down (pdf_doc *p)
{
  pdf_olitem *item, *first;

  item = p->outlines.current;
  if (!item->dict) {
    pdf_obj *tcolor, *action;

    WARN("Empty bookmark node!");
    WARN("You have tried to jump more than 1 level.");

    item->dict = texpdf_new_dict();

#define TITLE_STRING "<No Title>"
    texpdf_add_dict(item->dict,
                 texpdf_new_name("Title"),
                 texpdf_new_string(TITLE_STRING, strlen(TITLE_STRING)));

    tcolor = texpdf_new_array();
    texpdf_add_array(tcolor, texpdf_new_number(1.0));
    texpdf_add_array(tcolor, texpdf_new_number(0.0));
    texpdf_add_array(tcolor, texpdf_new_number(0.0));
    texpdf_add_dict (item->dict,
                  texpdf_new_name("C"), texpdf_link_obj(tcolor));
    texpdf_release_obj(tcolor);

    texpdf_add_dict (item->dict,
                  texpdf_new_name("F"), texpdf_new_number(1.0));

#define JS_CODE "app.alert(\"The author of this document made this bookmark item empty!\", 3, 0)"
    action = texpdf_new_dict();
    texpdf_add_dict(action,
                 texpdf_new_name("S"), texpdf_new_name("JavaScript"));
    texpdf_add_dict(action, 
                 texpdf_new_name("JS"), texpdf_new_string(JS_CODE, strlen(JS_CODE)));
    texpdf_add_dict(item->dict,
                 texpdf_new_name("A"), texpdf_link_obj(action));
    texpdf_release_obj(action);
  }

  item->first    = first = NEW(1, pdf_olitem);
  first->dict    = NULL;
  first->is_open = 0;
  first->parent  = item;
  first->next    = NULL;
  first->first   = NULL;

  p->outlines.current = first;
  p->outlines.current_depth++;

  return 0;
}

int
texpdf_doc_bookmarks_depth (pdf_doc *p)
{
  return p->outlines.current_depth;
}

void
texpdf_doc_bookmarks_add (pdf_doc *p, pdf_obj *dict, int is_open)
{
  pdf_olitem *item, *next;

  ASSERT(p && dict);

  item = p->outlines.current;

  if (!item) {
    item = NEW(1, pdf_olitem);
    item->parent = NULL;
    p->outlines.first = item;
  } else if (item->dict) { /* go to next item */
    item = item->next;
  }

#define BMOPEN(b,p) (((b) < 0) ? (((p)->outlines.current_depth > (p)->opt.outline_open_depth) ? 0 : 1) : (b))

#if 0
  item->dict    = texpdf_link_obj(dict);
#endif
  item->dict    = dict; 
  item->first   = NULL;
  item->is_open = BMOPEN(is_open, p);

  item->next    = next = NEW(1, pdf_olitem);
  next->dict    = NULL;
  next->parent  = item->parent;
  next->first   = NULL;
  next->is_open = -1;
  next->next    = NULL;

  p->outlines.current = item;

  pdf_doc_add_goto(p, dict);

  return;
}

static void
pdf_doc_close_bookmarks (pdf_doc *p)
{
  pdf_obj     *catalog = p->root.dict;
  pdf_olitem  *item;
  int          count;
  pdf_obj     *bm_root, *bm_root_ref;
  
  item = p->outlines.first;
  if (item->dict) {
    bm_root     = texpdf_new_dict();
    bm_root_ref = texpdf_ref_obj(bm_root);
    count       = flush_bookmarks(item, bm_root_ref, bm_root);
    texpdf_add_dict(bm_root,
                 texpdf_new_name("Count"),
                 texpdf_new_number(count));
    texpdf_add_dict(catalog,
                 texpdf_new_name("Outlines"),
                 bm_root_ref);
    texpdf_release_obj(bm_root);
  }
  clean_bookmarks(item);

  p->outlines.first   = NULL;
  p->outlines.current = NULL;
  p->outlines.current_depth = 0;

  return;
}


static const char *name_dict_categories[] = {
  "Dests", "AP", "JavaScript", "Pages",
  "Templates", "IDS", "URLS", "EmbeddedFiles",
  "AlternatePresentations", "Renditions"
};
#define NUM_NAME_CATEGORY (sizeof(name_dict_categories)/sizeof(name_dict_categories[0]))

static void
pdf_doc_init_names (pdf_doc *p, int check_gotos)
{
  int    i;

  p->root.names   = NULL;
  
  p->names = NEW(NUM_NAME_CATEGORY + 1, struct name_dict);
  for (i = 0; i < NUM_NAME_CATEGORY; i++) {
    p->names[i].category = name_dict_categories[i];
    p->names[i].data     = strcmp(name_dict_categories[i], "Dests") ?
                             NULL : texpdf_new_name_tree();
    /*
     * We need a non-null entry for PDF destinations in order to find
     * broken links even if no destination is defined in the DVI file.
     */
  }
  p->names[NUM_NAME_CATEGORY].category = NULL;
  p->names[NUM_NAME_CATEGORY].data     = NULL;

  p->check_gotos   = check_gotos;
  texpdf_ht_init_table(&p->gotos, (void (*) (void *)) texpdf_release_obj);

  return;
}

int
texpdf_doc_add_names (pdf_doc *p, const char *category,
                   const void *key, int keylen, pdf_obj *value)
{
  int      i;

  for (i = 0; p->names[i].category != NULL; i++) {
    if (!strcmp(p->names[i].category, category)) {
      break;
    }
  }
  if (p->names[i].category == NULL) {
    WARN("Unknown name dictionary category \"%s\".", category);
    return -1;
  }
  if (!p->names[i].data) {
    p->names[i].data = texpdf_new_name_tree();
  }

  return texpdf_names_add_object(p->names[i].data, key, keylen, value);
}

static void
pdf_doc_add_goto (pdf_doc *p, pdf_obj *annot_dict)
{
  pdf_obj *subtype = NULL, *A = NULL, *S = NULL, *D = NULL, *D_new, *dict;
  const char *dest, *key;

  if (!p->check_gotos)
    return;

  /*
   * An annotation dictionary coming from an annotation special
   * must have a "Subtype". An annotation dictionary coming from
   * an outline special has none.
   */
  subtype = pdf_deref_obj(texpdf_lookup_dict(annot_dict, "Subtype"));
  if (subtype) {
    if (PDF_OBJ_UNDEFINED(subtype))
      goto undefined;
    else if (!PDF_OBJ_NAMETYPE(subtype))
      goto error;
    else if (strcmp(texpdf_name_value(subtype), "Link"))
      goto cleanup;
  }

  dict = annot_dict;
  key = "Dest";
  D = pdf_deref_obj(texpdf_lookup_dict(annot_dict, key));
  if (PDF_OBJ_UNDEFINED(D))
    goto undefined;

  A = pdf_deref_obj(texpdf_lookup_dict(annot_dict, "A"));
  if (A) {
    if (PDF_OBJ_UNDEFINED(A))
      goto undefined;
    else if (D || !PDF_OBJ_DICTTYPE(A))
      goto error;
    else {
      S = pdf_deref_obj(texpdf_lookup_dict(A, "S"));
      if (PDF_OBJ_UNDEFINED(S))
	goto undefined;
      else if (!PDF_OBJ_NAMETYPE(S))
	goto error;
      else if (strcmp(texpdf_name_value(S), "GoTo"))
	goto cleanup;

      dict = A;
      key = "D";
      D = pdf_deref_obj(texpdf_lookup_dict(A, key));
    }
  }

  if (PDF_OBJ_STRINGTYPE(D))
    dest = (char *) texpdf_string_value(D);
#if 0
  /* Names as destinations are not supported by dvipdfmx */
  else if (PDF_OBJ_NAMETYPE(D))
    dest = texpdf_name_value(D);
#endif
  else if (PDF_OBJ_ARRAYTYPE(D))
    goto cleanup;
  else if (PDF_OBJ_UNDEFINED(D))
    goto undefined;
  else
    goto error;

  D_new = texpdf_ht_lookup_table(&p->gotos, dest, strlen(dest));
  if (!D_new) {
    char buf[10];

    /* We use hexadecimal notation for our numeric destinations.
     * Other bases (e.g., 10+26 or 10+2*26) would be more efficient.
     */
    sprintf(buf, "%lx", ht_table_size(&p->gotos));
    D_new = texpdf_new_string(buf, strlen(buf));
    texpdf_ht_append_table(&p->gotos, dest, strlen(dest), D_new);
  }

  {
    pdf_obj *key_obj = texpdf_new_name(key);
    if (!texpdf_add_dict(dict, key_obj, texpdf_link_obj(D_new)))
      texpdf_release_obj(key_obj);
  }

 cleanup:
  if (subtype)
    texpdf_release_obj(subtype);
  if (A)
    texpdf_release_obj(A);
  if (S)
    texpdf_release_obj(S);
  if (D)
    texpdf_release_obj(D);

  return;

 error:
  WARN("Unknown PDF annotation format. Output file may be broken.");
  goto cleanup;

 undefined:
  WARN("Cannot optimize PDF annotations. Output file may be broken."
       " Please restart with option \"-C 0x10\"\n");
  goto cleanup;
}

static void
warn_undef_dests (struct ht_table *dests, struct ht_table *gotos)
{
  struct ht_iter iter;

  if (ht_set_iter(gotos, &iter) < 0)
    return;

  do {
    int keylen;
    char *key = ht_iter_getkey(&iter, &keylen);
    if (!texpdf_ht_lookup_table(dests, key, keylen)) {
      char *dest = NEW(keylen+1, char);
      memcpy(dest, key, keylen);
      dest[keylen] = 0;
      WARN("PDF destination \"%s\" not defined.", dest);
      RELEASE(dest);
    }
  } while (ht_iter_next(&iter) >= 0);

  ht_clear_iter(&iter);
}

static void
pdf_doc_close_names (pdf_doc *p)
{
  pdf_obj  *tmp;
  int       i;

  for (i = 0; p->names[i].category != NULL; i++) {
    if (p->names[i].data) {
      struct ht_table *data = p->names[i].data;
      pdf_obj  *name_tree;
      long count;

      if (!p->check_gotos || strcmp(p->names[i].category, "Dests"))
	name_tree = texpdf_names_create_tree(data, &count, NULL);
      else {
	name_tree = texpdf_names_create_tree(data, &count, &p->gotos);

	if (verbose && count < data->count)
	  MESG("\nRemoved %ld unused PDF destinations\n", data->count-count);

	if (count < p->gotos.count)
	  warn_undef_dests(data, &p->gotos);
      }

      if (name_tree) {
        if (!p->root.names)
          p->root.names = texpdf_new_dict();
        texpdf_add_dict(p->root.names,
                     texpdf_new_name(p->names[i].category),
                     texpdf_ref_obj(name_tree));
        texpdf_release_obj(name_tree);
      }
      texpdf_delete_name_tree(&p->names[i].data);
    }
  }

  if (p->root.names) {
    tmp = texpdf_lookup_dict(p->root.dict, "Names");
    if (!tmp) {
      texpdf_add_dict(p->root.dict,
                   texpdf_new_name("Names"),
                   texpdf_ref_obj (p->root.names));
    } else if (PDF_OBJ_DICTTYPE(tmp)) {
      texpdf_merge_dict(p->root.names, tmp);
      texpdf_add_dict(p->root.dict,
                   texpdf_new_name("Names"),
                   texpdf_ref_obj (p->root.names));
    } else { /* Maybe reference */
      /* What should I do? */
      WARN("Could not modify Names dictionary.");
    }
    texpdf_release_obj(p->root.names);
    p->root.names = NULL;
  }

  RELEASE(p->names);
  p->names = NULL;

  texpdf_ht_clear_table(&p->gotos);

  return;
}


void
texpdf_doc_add_annot (pdf_doc *p, unsigned page_no, const pdf_rect *rect,
		   pdf_obj *annot_dict, int new_annot)
{
  pdf_page *page;
  pdf_obj  *rect_array;
  double    annot_grow = p->opt.annot_grow;
  double    xpos, ypos;
  pdf_rect  annbox;

  page = doc_get_page_entry(p, page_no);
  if (!page->annots)
    page->annots = texpdf_new_array();

  {
    pdf_rect  mediabox;

    texpdf_doc_get_mediabox(p, page_no, &mediabox);
    texpdf_dev_get_coord(&xpos, &ypos);
    annbox.llx = rect->llx - xpos; annbox.lly = rect->lly - ypos;
    annbox.urx = rect->urx - xpos; annbox.ury = rect->ury - ypos;

    if (annbox.llx < mediabox.llx || annbox.urx > mediabox.urx ||
        annbox.lly < mediabox.lly || annbox.ury > mediabox.ury) {
      WARN("Annotation out of page boundary.");
      WARN("Current page's MediaBox: [%g %g %g %g]",
           mediabox.llx, mediabox.lly, mediabox.urx, mediabox.ury);
      WARN("Annotation: [%g %g %g %g]",
           annbox.llx, annbox.lly, annbox.urx, annbox.ury);
      WARN("Maybe incorrect paper size specified.");
    }
    if (annbox.llx > annbox.urx || annbox.lly > annbox.ury) {
      WARN("Rectangle with negative width/height: [%g %g %g %g]",
           annbox.llx, annbox.lly, annbox.urx, annbox.ury);
    }
  }

  rect_array = texpdf_new_array();
  texpdf_add_array(rect_array, texpdf_new_number(ROUND(annbox.llx - annot_grow, 0.001)));
  texpdf_add_array(rect_array, texpdf_new_number(ROUND(annbox.lly - annot_grow, 0.001)));
  texpdf_add_array(rect_array, texpdf_new_number(ROUND(annbox.urx + annot_grow, 0.001)));
  texpdf_add_array(rect_array, texpdf_new_number(ROUND(annbox.ury + annot_grow, 0.001)));
  texpdf_add_dict (annot_dict, texpdf_new_name("Rect"), rect_array);

  texpdf_add_array(page->annots, texpdf_ref_obj(annot_dict));

  if (new_annot)
    pdf_doc_add_goto(p, annot_dict);

  return;
}


/*
 * PDF Article Thread
 */
static void
pdf_doc_init_articles (pdf_doc *p)
{
  p->root.threads = NULL;

  p->articles.num_entries = 0;
  p->articles.max_entries = 0;
  p->articles.entries     = NULL;

  return;
}

void
texpdf_doc_begin_article (pdf_doc *p, const char *article_id, pdf_obj *article_info)
{
  pdf_article *article;

  if (article_id == NULL || strlen(article_id) == 0)
    ERROR("Article thread without internal identifier.");

  if (p->articles.num_entries >= p->articles.max_entries) {
    p->articles.max_entries += PDFDOC_ARTICLE_ALLOC_SIZE;
    p->articles.entries = RENEW(p->articles.entries,
                                p->articles.max_entries, struct pdf_article);
  }
  article = &(p->articles.entries[p->articles.num_entries]);

  article->id = NEW(strlen(article_id)+1, char);
  strcpy(article->id, article_id);
  article->info = article_info;
  article->num_beads = 0;
  article->max_beads = 0;
  article->beads     = NULL;

  p->articles.num_entries++;

  return;
}

#if 0
void
pdf_doc_end_article (const char *article_id)
{
  return; /* no-op */
}
#endif

static pdf_bead *
find_bead (pdf_article *article, const char *bead_id)
{
  pdf_bead *bead;
  long      i;

  bead = NULL;
  for (i = 0; i < article->num_beads; i++) {
    if (!strcmp(article->beads[i].id, bead_id)) {
      bead = &(article->beads[i]);
      break;
    }
  }

  return bead;
}

void
texpdf_doc_add_bead (pdf_doc* p, const char *article_id,
                  const char *bead_id, long page_no, const pdf_rect *rect)
{
  pdf_article *article;
  pdf_bead    *bead;
  long         i;

  if (!article_id) {
    ERROR("No article identifier specified.");
  }

  article = NULL;
  for (i = 0; i < p->articles.num_entries; i++) {
    if (!strcmp(p->articles.entries[i].id, article_id)) {
      article = &(p->articles.entries[i]);
      break;
    }
  }
  if (!article) {
    ERROR("Specified article thread that doesn't exist.");
    return;
  }

  bead = bead_id ? find_bead(article, bead_id) : NULL;
  if (!bead) {
    if (article->num_beads >= article->max_beads) {
      article->max_beads += PDFDOC_BEAD_ALLOC_SIZE;
      article->beads = RENEW(article->beads,
                             article->max_beads, struct pdf_bead);
      for (i = article->num_beads; i < article->max_beads; i++) {
        article->beads[i].id = NULL;
        article->beads[i].page_no = -1;
      }
    }
    bead = &(article->beads[article->num_beads]);
    if (bead_id) {
      bead->id = NEW(strlen(bead_id)+1, char);
      strcpy(bead->id, bead_id);
    } else {
      bead->id = NULL;
    }
    article->num_beads++;
  }
  bead->rect.llx = rect->llx;
  bead->rect.lly = rect->lly;
  bead->rect.urx = rect->urx;
  bead->rect.ury = rect->ury;
  bead->page_no  = page_no;

  return;
}

static pdf_obj *
make_article (pdf_doc *p,
              pdf_article *article,
              const char **bead_ids, int num_beads,
              pdf_obj *article_info)
{
  pdf_obj *art_dict;
  pdf_obj *first, *prev, *last;
  long     i, n;

  if (!article)
    return NULL;

  art_dict = texpdf_new_dict();
  first = prev = last = NULL;
  /*
   * The bead_ids represents logical order of beads in an article thread.
   * If bead_ids is not given, we create an article thread in the order of
   * beads appeared.
   */
  n = bead_ids ? num_beads : article->num_beads;
  for (i = 0; i < n; i++) {
    pdf_bead *bead;

    bead = bead_ids ? find_bead(article, bead_ids[i]) : &(article->beads[i]);
    if (!bead || bead->page_no < 0) {
      continue;
    }
    last = texpdf_new_dict();
    if (prev == NULL) {
      first = last;
      texpdf_add_dict(first,
                   texpdf_new_name("T"), texpdf_ref_obj(art_dict));
    } else {
      texpdf_add_dict(prev,
                   texpdf_new_name("N"), texpdf_ref_obj(last));
      texpdf_add_dict(last,
                   texpdf_new_name("V"), texpdf_ref_obj(prev));
      /* We must link first to last. */
      if (prev != first)
        texpdf_release_obj(prev);
    }

    /* Realize bead now. */
    {
      pdf_page *page;
      pdf_obj  *rect;

      page = doc_get_page_entry(p, bead->page_no);
      if (!page->beads) {
        page->beads = texpdf_new_array();
      }
      texpdf_add_dict(last, texpdf_new_name("P"), texpdf_link_obj(page->page_ref));
      rect = texpdf_new_array();
      texpdf_add_array(rect, texpdf_new_number(ROUND(bead->rect.llx, 0.01)));
      texpdf_add_array(rect, texpdf_new_number(ROUND(bead->rect.lly, 0.01)));
      texpdf_add_array(rect, texpdf_new_number(ROUND(bead->rect.urx, 0.01)));
      texpdf_add_array(rect, texpdf_new_number(ROUND(bead->rect.ury, 0.01)));
      texpdf_add_dict (last, texpdf_new_name("R"), rect);
      texpdf_add_array(page->beads, texpdf_ref_obj(last));
    }

    prev = last;
  }

  if (first && last) {
    texpdf_add_dict(last,
                 texpdf_new_name("N"), texpdf_ref_obj(first));
    texpdf_add_dict(first,
                 texpdf_new_name("V"), texpdf_ref_obj(last));
    if (first != last) {
      texpdf_release_obj(last);
    }
    texpdf_add_dict(art_dict,
                 texpdf_new_name("F"), texpdf_ref_obj(first));
    /* If article_info is supplied, we override article->info. */
    if (article_info) {
      texpdf_add_dict(art_dict,
                   texpdf_new_name("I"), article_info);
    } else if (article->info) {
      texpdf_add_dict(art_dict,
                   texpdf_new_name("I"), texpdf_ref_obj(article->info));
      texpdf_release_obj(article->info);
      article->info = NULL; /* We do not write as object reference. */
    }
    texpdf_release_obj(first);
  } else {
    texpdf_release_obj(art_dict);
    art_dict = NULL;
  }

  return art_dict;
}

static void
clean_article (pdf_article *article)
{
  if (!article)
    return;
    
  if (article->beads) {
    long  i;

    for (i = 0; i < article->num_beads; i++) {
      if (article->beads[i].id)
        RELEASE(article->beads[i].id);
    }
    RELEASE(article->beads);
    article->beads = NULL;
  }
    
  if (article->id)
    RELEASE(article->id);
  article->id = NULL;
  article->num_beads = 0;
  article->max_beads = 0;

  return;
}

static void
pdf_doc_close_articles (pdf_doc *p)
{
  int  i;

  for (i = 0; i < p->articles.num_entries; i++) {
    pdf_article *article;

    article = &(p->articles.entries[i]);
    if (article->beads) {
      pdf_obj *art_dict;

      art_dict = make_article(p, article, NULL, 0, NULL);
      if (!p->root.threads) {
        p->root.threads = texpdf_new_array();
      }
      texpdf_add_array(p->root.threads, texpdf_ref_obj(art_dict));
      texpdf_release_obj(art_dict);
    }
    clean_article(article);
  }
  RELEASE(p->articles.entries);
  p->articles.entries = NULL;
  p->articles.num_entries = 0;
  p->articles.max_entries = 0;

  if (p->root.threads) {
    texpdf_add_dict(p->root.dict,
                 texpdf_new_name("Threads"),
                 texpdf_ref_obj (p->root.threads));
    texpdf_release_obj(p->root.threads);
    p->root.threads = NULL;
  }

  return;
}

/* page_no = 0 for root page tree node. */
void
texpdf_doc_set_mediabox (pdf_doc *p, unsigned page_no, const pdf_rect *mediabox)
{
  pdf_page *page;

  if (page_no == 0) {
    p->pages.mediabox.llx = mediabox->llx;
    p->pages.mediabox.lly = mediabox->lly;
    p->pages.mediabox.urx = mediabox->urx;
    p->pages.mediabox.ury = mediabox->ury;
  } else {
    page = doc_get_page_entry(p, page_no);
    page->cropbox.llx = mediabox->llx;
    page->cropbox.lly = mediabox->lly;
    page->cropbox.urx = mediabox->urx;
    page->cropbox.ury = mediabox->ury;
    page->flags |= USE_MY_MEDIABOX;
  }

  return;
}

void
texpdf_doc_get_mediabox (pdf_doc *p, unsigned page_no, pdf_rect *mediabox)
{
  pdf_page *page;

  if (page_no == 0) {
    mediabox->llx = p->pages.mediabox.llx;
    mediabox->lly = p->pages.mediabox.lly;
    mediabox->urx = p->pages.mediabox.urx;
    mediabox->ury = p->pages.mediabox.ury;
  } else {
    page = doc_get_page_entry(p, page_no);
    if (page->flags & USE_MY_MEDIABOX) {
      mediabox->llx = page->cropbox.llx;
      mediabox->lly = page->cropbox.lly;
      mediabox->urx = page->cropbox.urx;
      mediabox->ury = page->cropbox.ury;
    } else {
      mediabox->llx = p->pages.mediabox.llx;
      mediabox->lly = p->pages.mediabox.lly;
      mediabox->urx = p->pages.mediabox.urx;
      mediabox->ury = p->pages.mediabox.ury;
    }
  }

  return;
}

pdf_obj *
texpdf_doc_current_page_resources (pdf_doc *p)
{
  pdf_obj  *resources;
  pdf_page *currentpage;

  if (p->pending_forms) {
    if (p->pending_forms->form.resources) {
      resources = p->pending_forms->form.resources;
    } else {
      resources = p->pending_forms->form.resources = texpdf_new_dict();
    }
  } else {
    currentpage = LASTPAGE(p);
    if (currentpage->resources) {
      resources = currentpage->resources;
    } else {
      resources = currentpage->resources = texpdf_new_dict();
    }
  }

  return resources;
}

pdf_obj *
texpdf_doc_get_dictionary (pdf_doc *p, const char *category)
{
  pdf_obj *dict = NULL;

  ASSERT(category);

  if (!strcmp(category, "Names")) {
    if (!p->root.names)
      p->root.names = texpdf_new_dict();
    dict = p->root.names;
  } else if (!strcmp(category, "Pages")) {
    if (!p->root.pages)
      p->root.pages = texpdf_new_dict();
    dict = p->root.pages;
  } else if (!strcmp(category, "Catalog")) {
    if (!p->root.dict)
      p->root.dict = texpdf_new_dict();
    dict = p->root.dict;
  } else if (!strcmp(category, "Info")) {
    if (!p->info)
      p->info = texpdf_new_dict();
    dict = p->info;
  } else if (!strcmp(category, "@THISPAGE")) {
    /* Sorry for this... */
    pdf_page *currentpage;

    currentpage = LASTPAGE(p);
    dict =  currentpage->page_obj;
  }

  if (!dict) {
    ERROR("Document dict. \"%s\" not exist. ", category);
  }

  return dict;
}

long
texpdf_doc_current_page_number (pdf_doc *p)
{
  return (long) (PAGECOUNT(p) + 1);
}

pdf_obj *
texpdf_doc_ref_page (pdf_doc *p, unsigned long page_no)
{
  pdf_page *page;

  page = doc_get_page_entry(p, page_no);
  if (!page->page_obj) {
    page->page_obj = texpdf_new_dict();
    page->page_ref = texpdf_ref_obj(page->page_obj);
  }

  return texpdf_link_obj(page->page_ref);
}

pdf_obj *
texpdf_doc_get_reference (pdf_doc *p, const char *category)
{
  pdf_obj *ref = NULL;
  long     page_no;

  ASSERT(category);

  page_no = texpdf_doc_current_page_number(p);
  if (!strcmp(category, "@THISPAGE")) {
    ref = texpdf_doc_ref_page(p, page_no);
  } else if (!strcmp(category, "@PREVPAGE")) {
    if (page_no <= 1) {
      ERROR("Reference to previous page, but no pages have been completed yet.");
    }
    ref = texpdf_doc_ref_page(p, page_no - 1);
  } else if (!strcmp(category, "@NEXTPAGE")) {
    ref = texpdf_doc_ref_page(p, page_no + 1);
  }

  if (!ref) {
    ERROR("Reference to \"%s\" not exist. ", category);
  }

  return ref;
}

static void
pdf_doc_new_page (pdf_doc *p)
{
  pdf_page *currentpage;

  if (PAGECOUNT(p) >= MAXPAGES(p)) {
    doc_resize_page_entries(p, MAXPAGES(p) + PDFDOC_PAGES_ALLOC_SIZE);
  }

  /*
   * This is confusing. pdf_doc_finish_page() have increased page count!
   */
  currentpage = LASTPAGE(p);
  /* Was this page already instantiated by a forward reference to it? */
  if (!currentpage->page_ref) {
    currentpage->page_obj = texpdf_new_dict();
    currentpage->page_ref = texpdf_ref_obj(currentpage->page_obj);
  }

  currentpage->background = NULL;
  currentpage->contents   = texpdf_new_stream(STREAM_COMPRESS);
  currentpage->resources  = texpdf_new_dict();

  currentpage->annots = NULL;
  currentpage->beads  = NULL;

  return;
}

/* This only closes contents and resources. */
static void
pdf_doc_finish_page (pdf_doc *p)
{
  pdf_page *currentpage;

  if (p->pending_forms) {
    ERROR("A pending form XObject at the end of page.");
  }

  currentpage = LASTPAGE(p);
  if (!currentpage->page_obj)
    currentpage->page_obj = texpdf_new_dict();

  /*
   * Make Contents array.
   */

  /*
   * Global BOP content stream.
   * texpdf_ref_obj() returns reference itself when the object is
   * indirect reference, not reference to the indirect reference.
   * We keep bop itself but not reference to it since it is
   * expected to be small.
   */
  if (p->pages.bop &&
      pdf_stream_length(p->pages.bop) > 0) {
    currentpage->content_refs[0] = texpdf_ref_obj(p->pages.bop);
  } else {
    currentpage->content_refs[0] = NULL;
  }
  /*
   * Current page background content stream.
   */
  if (currentpage->background) {
    if (pdf_stream_length(currentpage->background) > 0) {
      currentpage->content_refs[1] = texpdf_ref_obj(currentpage->background);
      texpdf_add_stream (currentpage->background, "\n", 1);
    }
    texpdf_release_obj(currentpage->background);
    currentpage->background = NULL;
  } else {
    currentpage->content_refs[1] = NULL;
  }

  /* Content body of current page */
  currentpage->content_refs[2] = texpdf_ref_obj(currentpage->contents);
  texpdf_add_stream (currentpage->contents, "\n", 1);
  texpdf_release_obj(currentpage->contents);
  currentpage->contents = NULL;

  /*
   * Global EOP content stream.
   */
  if (p->pages.eop &&
      pdf_stream_length(p->pages.eop) > 0) {
    currentpage->content_refs[3] = texpdf_ref_obj(p->pages.eop);
  } else {
    currentpage->content_refs[3] = NULL;
  }

  /*
   * Page resources.
   */
  if (currentpage->resources) {
    pdf_obj *procset;
    /*
     * ProcSet is obsolete in PDF-1.4 but recommended for compatibility.
     */

    procset = texpdf_new_array ();
    texpdf_add_array(procset, texpdf_new_name("PDF"));
    texpdf_add_array(procset, texpdf_new_name("Text"));
    texpdf_add_array(procset, texpdf_new_name("ImageC"));
    texpdf_add_array(procset, texpdf_new_name("ImageB"));
    texpdf_add_array(procset, texpdf_new_name("ImageI"));
    texpdf_add_dict(currentpage->resources, texpdf_new_name("ProcSet"), procset);

    texpdf_add_dict(currentpage->page_obj,
                 texpdf_new_name("Resources"),
                 texpdf_ref_obj(currentpage->resources));
    texpdf_release_obj(currentpage->resources);
    currentpage->resources = NULL;
  }

  if (p->manual_thumb_enabled) {
    char    *thumb_filename;
    pdf_obj *thumb_ref;

    thumb_filename = NEW(strlen(thumb_basename)+7, char);
    sprintf(thumb_filename, "%s.%ld",
            thumb_basename, (p->pages.num_entries % 99999) + 1L);
    thumb_ref = read_thumbnail(p, thumb_filename);
    RELEASE(thumb_filename);
    if (thumb_ref)
      texpdf_add_dict(currentpage->page_obj, texpdf_new_name("Thumb"), thumb_ref);
  }

  p->pages.num_entries++;

  return;
}

void
texpdf_doc_set_bgcolor (pdf_doc *p, const pdf_color *color)
{
  if (color)
    texpdf_color_copycolor(&p->bgcolor, color);
  else { /* as clear... */
    texpdf_color_white(&p->bgcolor);
  }
}

static void
doc_fill_page_background (pdf_doc *p)
{
  pdf_page  *currentpage;
  pdf_rect   r;
  int        cm;
  pdf_obj   *saved_content;

  cm = texpdf_dev_get_param(PDF_DEV_PARAM_COLORMODE);
  if (!cm || texpdf_color_is_white(&p->bgcolor)) {
    return;
  }

  texpdf_doc_get_mediabox(p, texpdf_doc_current_page_number(p), &r);

  currentpage = LASTPAGE(p);
  ASSERT(currentpage);

  if (!currentpage->background)
    currentpage->background = texpdf_new_stream(STREAM_COMPRESS);

  saved_content = currentpage->contents;
  currentpage->contents = currentpage->background;

  texpdf_dev_gsave(p);
  texpdf_dev_set_nonstrokingcolor(p, &p->bgcolor);
  texpdf_dev_rectfill(p, r.llx, r.lly, r.urx - r.llx, r.ury - r.lly);
  texpdf_dev_grestore(p);

  currentpage->contents = saved_content;

  return;
}

void
texpdf_doc_begin_page (pdf_doc *p, double scale, double x_origin, double y_origin)
{
  pdf_tmatrix  M;

  M.a = scale; M.b = 0.0;
  M.c = 0.0  ; M.d = scale;
  M.e = x_origin;
  M.f = y_origin;

  /* pdf_doc_new_page() allocates page content stream. */
  pdf_doc_new_page(p);
  texpdf_dev_bop(p, &M);

  return;
}

void
texpdf_doc_end_page (pdf_doc *p)
{
  texpdf_dev_eop(p);
  doc_fill_page_background(p);

  pdf_doc_finish_page(p);

  return;
}

void
texpdf_doc_add_page_content (pdf_doc *p, const char *buffer, unsigned length)
{
  pdf_page *currentpage;

  if (p->pending_forms) {
    texpdf_add_stream(p->pending_forms->form.contents, buffer, length);
  } else {
    currentpage = LASTPAGE(p);
    texpdf_add_stream(currentpage->contents, buffer, length);
  }

  return;
}

static void pdf_init(pdf_doc *p)
{
  memset(p, 0, sizeof(pdf_doc));
  p->bgcolor.num_components = 1;
  p->bgcolor.spot_color_name = NULL;
  p->bgcolor.values[0] = 1.0;
}

void 
texpdf_doc_free(pdf_doc *p) {
  // XXX
  free(p);
}

pdf_doc *
texpdf_open_document (const char *filename,
		   int do_encryption,
                   double media_width, double media_height,
                   double annot_grow_amount, int bookmark_open_depth,
                   int check_gotos)
{
  pdf_doc *p = malloc(sizeof(pdf_doc));
  pdf_init(p);
  pdf_out_init(filename, do_encryption);

  pdf_doc_init_catalog(p);

  p->opt.annot_grow = annot_grow_amount;
  p->opt.outline_open_depth = bookmark_open_depth;

  texpdf_init_resources();
  texpdf_init_colors();
  texpdf_init_fonts();
  /* Thumbnail want this to be initialized... */
  texpdf_init_images();

  pdf_doc_init_docinfo(p);

  pdf_doc_init_bookmarks(p, bookmark_open_depth);
  pdf_doc_init_articles (p);
  pdf_doc_init_names    (p, check_gotos);
  pdf_doc_init_page_tree(p, media_width, media_height);

  texpdf_doc_set_bgcolor(p, NULL);

  if (do_encryption) {
    pdf_obj *encrypt = pdf_encrypt_obj();
    texpdf_set_encrypt(encrypt);
    texpdf_release_obj(encrypt);
  }
  texpdf_set_id(texpdf_enc_id_array());

  /* Create a default name for thumbnail image files */
  if (p->manual_thumb_enabled) {
    if (strlen(filename) > 4 &&
        !strncmp(".pdf", filename + strlen(filename) - 4, 4)) {
      thumb_basename = NEW(strlen(filename)-4+1, char);
      strncpy(thumb_basename, filename, strlen(filename)-4);
      thumb_basename[strlen(filename)-4] = 0;
    } else {
      thumb_basename = NEW(strlen(filename)+1, char);
      strcpy(thumb_basename, filename);
    }
  }

  p->pending_forms = NULL;
   
  return p;
}

void
texpdf_doc_set_creator (pdf_doc *p, const char *creator)
{
  if (!creator ||
      creator[0] == '\0')
    return;

  texpdf_add_dict(p->info,
               texpdf_new_name("Creator"),
               texpdf_new_string(creator, strlen(creator)));
}


void
texpdf_close_document (pdf_doc *p)
{

  /*
   * Following things were kept around so user can add dictionary items.
   */
  pdf_doc_close_articles (p);
  pdf_doc_close_names    (p);
  pdf_doc_close_bookmarks(p);
  pdf_doc_close_page_tree(p);
  pdf_doc_close_docinfo  (p);

  pdf_doc_close_catalog  (p);

  texpdf_close_images();
  texpdf_close_fonts ();
  texpdf_close_colors();

  texpdf_close_resources(); /* Should be at last. */

  pdf_out_flush();

  if (thumb_basename)
    RELEASE(thumb_basename);

  return;
}

/*
 * All this routine does is give the form a name and add a unity scaling matrix.
 * It fills in required fields.  The caller must initialize the stream.
 */
static void
pdf_doc_make_xform (pdf_obj     *xform,
                    pdf_rect    *bbox,
                    pdf_tmatrix *matrix,
                    pdf_obj     *resources,
                    pdf_obj     *attrib)
{
  pdf_obj *xform_dict;
  pdf_obj *tmp;

  xform_dict = texpdf_stream_dict(xform);
  texpdf_add_dict(xform_dict,
               texpdf_new_name("Type"),     texpdf_new_name("XObject"));
  texpdf_add_dict(xform_dict,
               texpdf_new_name("Subtype"),  texpdf_new_name("Form"));
  texpdf_add_dict(xform_dict,
               texpdf_new_name("FormType"), texpdf_new_number(1.0));

  if (!bbox)
    ERROR("No BoundingBox supplied.");

  tmp = texpdf_new_array();
  texpdf_add_array(tmp, texpdf_new_number(ROUND(bbox->llx, .001)));
  texpdf_add_array(tmp, texpdf_new_number(ROUND(bbox->lly, .001)));
  texpdf_add_array(tmp, texpdf_new_number(ROUND(bbox->urx, .001)));
  texpdf_add_array(tmp, texpdf_new_number(ROUND(bbox->ury, .001)));
  texpdf_add_dict(xform_dict, texpdf_new_name("BBox"), tmp);

  if (matrix) {
    tmp = texpdf_new_array();
    texpdf_add_array(tmp, texpdf_new_number(ROUND(matrix->a, .00001)));
    texpdf_add_array(tmp, texpdf_new_number(ROUND(matrix->b, .00001)));
    texpdf_add_array(tmp, texpdf_new_number(ROUND(matrix->c, .00001)));
    texpdf_add_array(tmp, texpdf_new_number(ROUND(matrix->d, .00001)));
    texpdf_add_array(tmp, texpdf_new_number(ROUND(matrix->e, .001  )));
    texpdf_add_array(tmp, texpdf_new_number(ROUND(matrix->f, .001  )));
    texpdf_add_dict(xform_dict, texpdf_new_name("Matrix"), tmp);
  }

  if (attrib) {
    texpdf_merge_dict(xform_dict, attrib);
  }

  texpdf_add_dict(xform_dict, texpdf_new_name("Resources"), resources);

  return;
}

/*
 * begin_form_xobj creates an xobject with its "origin" at
 * xpos and ypos that is clipped to the specified bbox. Note
 * that the origin is not the lower left corner of the bbox.
 */
int
texpdf_doc_begin_grabbing (pdf_doc *p, const char *ident,
                        double ref_x, double ref_y, const pdf_rect *cropbox)
{
  int         xobj_id = -1;
  pdf_form   *form;
  struct form_list_node *fnode;
  xform_info  info;

  texpdf_dev_push_gstate();

  fnode = NEW(1, struct form_list_node);

  fnode->prev    = p->pending_forms;
  fnode->q_depth = texpdf_dev_current_depth();
  form           = &fnode->form;

  /*
  * The reference point of an Xobject is at the lower left corner
  * of the bounding box.  Since we would like to have an arbitrary
  * reference point, we use a transformation matrix, translating
  * the reference point to (0,0).
  */

  form->matrix.a = 1.0; form->matrix.b = 0.0;
  form->matrix.c = 0.0; form->matrix.d = 1.0;
  form->matrix.e = -ref_x;
  form->matrix.f = -ref_y;

  form->cropbox.llx = ref_x + cropbox->llx;
  form->cropbox.lly = ref_y + cropbox->lly;
  form->cropbox.urx = ref_x + cropbox->urx;
  form->cropbox.ury = ref_y + cropbox->ury;

  form->contents  = texpdf_new_stream(STREAM_COMPRESS);
  form->resources = texpdf_new_dict();

  texpdf_ximage_init_form_info(&info);

  info.matrix.a = 1.0; info.matrix.b = 0.0;
  info.matrix.c = 0.0; info.matrix.d = 1.0;
  info.matrix.e = -ref_x;
  info.matrix.f = -ref_y;

  info.bbox.llx = cropbox->llx;
  info.bbox.lly = cropbox->lly;
  info.bbox.urx = cropbox->urx;
  info.bbox.ury = cropbox->ury;

  /* Use reference since content itself isn't available yet. */
  xobj_id = texpdf_ximage_defineresource(ident,
                                      PDF_XOBJECT_TYPE_FORM,
                                      &info, texpdf_ref_obj(form->contents));

  p->pending_forms = fnode;

  /*
   * Make sure the object is self-contained by adding the
   * current font and color to the object stream.
   */
  texpdf_dev_reset_fonts(1);
  texpdf_dev_reset_color(p, 1);  /* force color operators to be added to stream */

  return xobj_id;
}

void
texpdf_doc_end_grabbing (pdf_doc *p, pdf_obj *attrib)
{
  pdf_form *form;
  pdf_obj  *procset;
  struct form_list_node *fnode;

  if (!p->pending_forms) {
    WARN("Tried to close a nonexistent form XOject.");
    return;
  }
  
  fnode = p->pending_forms;
  form  = &fnode->form;

  texpdf_dev_grestore_to(p, fnode->q_depth);

  /*
   * ProcSet is obsolete in PDF-1.4 but recommended for compatibility.
   */
  procset = texpdf_new_array();
  texpdf_add_array(procset, texpdf_new_name("PDF"));
  texpdf_add_array(procset, texpdf_new_name("Text"));
  texpdf_add_array(procset, texpdf_new_name("ImageC"));
  texpdf_add_array(procset, texpdf_new_name("ImageB"));
  texpdf_add_array(procset, texpdf_new_name("ImageI"));
  texpdf_add_dict (form->resources, texpdf_new_name("ProcSet"), procset);

  pdf_doc_make_xform(form->contents,
                     &form->cropbox, &form->matrix,
                     texpdf_ref_obj(form->resources), attrib);
  texpdf_release_obj(form->resources);
  texpdf_release_obj(form->contents);
  if (attrib) texpdf_release_obj(attrib);

  p->pending_forms = fnode->prev;

  texpdf_dev_pop_gstate();

  texpdf_dev_reset_fonts(1);
  texpdf_dev_reset_color(p, 0);

  RELEASE(fnode);

  return;
}

/* Urgh */
static struct
{
  int      dirty;
  int      broken;
  pdf_obj *annot_dict;
  pdf_rect rect;
} breaking_state = {0, 0, NULL, {0.0, 0.0, 0.0, 0.0}};

static void
reset_box (void)
{
  breaking_state.rect.llx = breaking_state.rect.lly =  HUGE_VAL;
  breaking_state.rect.urx = breaking_state.rect.ury = -HUGE_VAL;
  breaking_state.dirty    = 0;
}

void
texpdf_doc_begin_annot (pdf_doc *p, pdf_obj *dict) /* XXX */
{
  breaking_state.annot_dict = dict;
  breaking_state.broken = 0;
  reset_box();
}

void
texpdf_doc_end_annot (pdf_doc *p)
{
  texpdf_doc_break_annot(p);
  breaking_state.annot_dict = NULL;
}

void
texpdf_doc_break_annot (pdf_doc *p)
{
  if (breaking_state.dirty) {
    pdf_obj  *annot_dict;

    /* Copy dict */
    annot_dict = texpdf_new_dict();
    texpdf_merge_dict(annot_dict, breaking_state.annot_dict);
    texpdf_doc_add_annot(p, texpdf_doc_current_page_number(p), &(breaking_state.rect),
		      annot_dict, !breaking_state.broken);
    texpdf_release_obj(annot_dict);

    breaking_state.broken = 1;
  }
  reset_box();
}

void
texpdf_doc_expand_box (pdf_doc *p, const pdf_rect *rect)
{
  breaking_state.rect.llx = MIN(breaking_state.rect.llx, rect->llx);
  breaking_state.rect.lly = MIN(breaking_state.rect.lly, rect->lly);
  breaking_state.rect.urx = MAX(breaking_state.rect.urx, rect->urx);
  breaking_state.rect.ury = MAX(breaking_state.rect.ury, rect->ury);
  breaking_state.dirty    = 1;
}

#if 0
/* This should be number tree */
void
pdf_doc_set_pagelabel (long  pg_start,
                       const char *type,
                       const void *prefix, int prfx_len, long start)
{
  pdf_doc *p = &pdoc;
  pdf_obj *label_dict;

  if (!p->root.pagelabels)
    p->root.pagelabels = texpdf_new_array();

  label_dict = texpdf_new_dict();
  if (!type || type[0] == '\0') /* Set back to default. */
    texpdf_add_dict(label_dict, texpdf_new_name("S"),  texpdf_new_name("D"));
  else {
    if (type)
      texpdf_add_dict(label_dict, texpdf_new_name("S"), texpdf_new_name(type));
    if (prefix && prfx_len > 0)
      texpdf_add_dict(label_dict,
                   texpdf_new_name("P"),
                   texpdf_new_string(prefix, prfx_len));
    if (start != 1)
      texpdf_add_dict(label_dict,
                   texpdf_new_name("St"), texpdf_new_number(start));
  }

  texpdf_add_array(p->root.pagelabels, texpdf_new_number(pg_start));
  texpdf_add_array(p->root.pagelabels, label_dict);

  return;
}
#endif
