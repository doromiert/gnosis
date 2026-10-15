/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-view.h"
#include "select/browser.hpp"
#include <glib/gstdio.h>
#include <string>
CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView*);
namespace {
struct Print {
  GTask* task;
  GtkPrintDialog* dialog;
  GtkPrintSetup* setup=nullptr;
  GtkWindow* parent=nullptr;
  std::string directory, path;
  ~Print() {
    if(!path.empty())g_unlink(path.c_str());
    if(!directory.empty())g_rmdir(directory.c_str());
    if(setup)gtk_print_setup_unref(setup);
    g_clear_object(&parent);g_object_unref(dialog);g_object_unref(task);
  }
  void fail(GError* error) {g_task_return_error(task,error);delete this;}
};
class PDF final : public CefPdfPrintCallback {
public:
  explicit PDF(Print* job):job_(job){}
  void OnPdfPrintFinished(const CefString&,bool success) override {
    auto* job=job_;job_=nullptr;
    if(!success) {job->fail(g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"Chromium could not render this document for printing"));return;}
    auto* file=g_file_new_for_path(job->path.c_str());
    gtk_print_dialog_print_file(job->dialog,job->parent,job->setup,file,nullptr,
      +[](GObject* object,GAsyncResult* result,gpointer user) {
        auto* job=static_cast<Print*>(user);GError* error=nullptr;
        if(!gtk_print_dialog_print_file_finish(GTK_PRINT_DIALOG(object),result,&error)){job->fail(error);return;}
        g_task_return_pointer(job->task,gtk_print_setup_ref(job->setup),reinterpret_cast<GDestroyNotify>(gtk_print_setup_unref));delete job;
      },job);
    g_object_unref(file);
  }
private:
  Print* job_;
  IMPLEMENT_REFCOUNTING(PDF);
};
}
void ephy_engine_web_view_print(EphyEngineView* view,GtkPageSetup* page,GtkPrintSettings* settings,GAsyncReadyCallback cb,gpointer user) {
  auto* job=new Print{g_task_new(view,nullptr,cb,user),gtk_print_dialog_new()};
  if(auto* root=gtk_widget_get_root(GTK_WIDGET(view));root&&GTK_IS_WINDOW(root))job->parent=GTK_WINDOW(g_object_ref(root));
  gtk_print_dialog_set_page_setup(job->dialog,page);gtk_print_dialog_set_print_settings(job->dialog,settings);
  gtk_print_dialog_setup(job->dialog,job->parent,nullptr,+[](GObject* object,GAsyncResult* result,gpointer user) {
    auto* job=static_cast<Print*>(user);GError* error=nullptr;
    job->setup=gtk_print_dialog_setup_finish(GTK_PRINT_DIALOG(object),result,&error);
    if(!job->setup){job->fail(error);return;}
    auto* view=EPHY_ENGINE_VIEW(g_task_get_source_object(job->task));auto browser=ephy_engine_view_browser(view);
    if(!browser){job->fail(g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CLOSED,"The tab was closed"));return;}
    char* directory=g_dir_make_tmp("jupiter-print-XXXXXX",&error);
    if(!directory){job->fail(error);return;}
    job->directory=directory;g_free(directory);job->path=job->directory+"/document.pdf";
    auto* setup=gtk_print_setup_get_page_setup(job->setup);auto* paper=gtk_page_setup_get_paper_size(setup);
    CefPdfPrintSettings pdf;
    pdf.landscape=gtk_page_setup_get_orientation(setup)==GTK_PAGE_ORIENTATION_LANDSCAPE || gtk_page_setup_get_orientation(setup)==GTK_PAGE_ORIENTATION_REVERSE_LANDSCAPE;
    pdf.paper_width=gtk_paper_size_get_width(paper,GTK_UNIT_INCH);pdf.paper_height=gtk_paper_size_get_height(paper,GTK_UNIT_INCH);
    pdf.print_background=true;
    browser->GetHost()->PrintToPDF(job->path,pdf,new PDF(job));
  },job);
}
GtkPrintSetup* ephy_engine_web_view_print_finish(EphyEngineView*,GAsyncResult* result,GError** error) {
  return static_cast<GtkPrintSetup*>(g_task_propagate_pointer(G_TASK(result),error));
}
