/* paf.h - minimal umbrella header for fpsvita-ui: only the ScePaf-RE headers
 * the shell uses, skipping the std/thread ones that do not build on GCC. */
#ifndef _VDSUITE_USER_PAF_H
#define _VDSUITE_USER_PAF_H

/* paf_types: SceWChar16, SceChar8, basic typedefs */
#include <paf/paf_types.h>

/* C functions: sce_paf_memset, sce_paf_strtol, sce_paf_memmove, etc. */
#include <paf/std/stdlib.h>
#include <paf/std/stdio.h>
#include <paf/std/string.h>
#include <paf/std/wchar.h>

/* C++ std bridging: brings std:: into paf:: namespace */
#include <paf/std/std_bridge>
#include <paf/std/memory>
#include <paf/std/list>

/* Threading primitives needed by widget/common */
#include <paf/thread/mutex.h>

/* System / util */
#include <paf/system/system.h>
#include <paf/versatileparam/idparam.h>
#include <paf/versatileparam/resourceparam.h>
#include <paf/versatileparam/versatileparam.h>
#include <paf/versatileparam/versatileparammanager.h>

/* CXML */
#include <paf/cxml/cxml.h>

/* Common */
#include <paf/common/main_thread_call.h>
#include <paf/common/call_list.h>

/* Resource */
#include <paf/resource/resource_types.h>
#include <paf/resource/resource_obj.h>

/* Graphics */
#include <paf/graphics/graphics.h>
#include <paf/graphics/surface/surface_pool.h>
#include <paf/graphics/surface/surface_base.h>
#include <paf/graphics/surface/surface.h>

/* Widget */
#include <paf/widget/types.h>
#include <paf/widget/core/environment.h>
#include <paf/widget/core/event.h>
#include <paf/widget/core/handler.h>
#include <paf/widget/core/widget.h>
#include <paf/widget/w_scroll_view_base.h>
#include <paf/widget/w_button_base.h>
#include <paf/widget/w_button.h>
#include <paf/widget/w_scene.h>
#include <paf/widget/w_text.h>
#include <paf/widget/w_scroll_view.h>
#include <paf/widget/w_check_box.h>
#include <paf/widget/w_box.h>
#include <paf/widget/factory.h>

/* Top-level */
#include <paf/paf_cdlg.h>
#include <paf/plugin.h>
#include <paf/framework.h>

#endif /* _VDSUITE_USER_PAF_H */
