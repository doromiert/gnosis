/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-view.h"
#include "select/browser.hpp"
#include <glib/gstdio.h>
#include <string>
CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView*);
std::shared_ptr<GnosisBrave::Page> ephy_engine_view_brave_page(EphyEngineView*);
namespace {
struct Print {
  GTask* task;
  GtkPrintDialog* dialog;
  GtkPrintSetup* setup=nullptr;
  GtkWindow* parent=nullptr;
  GCancellable* cancel=nullptr;
  gulong unmap=0;
  std::string directory, path;
  ~Print() {
    if(unmap)g_signal_handler_disconnect(g_task_get_source_object(task),unmap);
    if(!path.empty())g_unlink(path.c_str());
    if(!directory.empty())g_rmdir(directory.c_str());
    if(setup)gtk_print_setup_unref(setup);
    g_clear_object(&cancel);g_clear_object(&parent);g_object_unref(dialog);g_object_unref(task);
  }
  void fail(GError* error) {g_task_return_error(task,error);delete this;}
  void rendered(bool success) {
    if(g_cancellable_is_cancelled(cancel)){fail(g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"Printing was cancelled"));return;}
    if(!success) {fail(g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,"The browser could not render this document for printing"));return;}
    auto* file=g_file_new_for_path(path.c_str());
    gtk_print_dialog_print_file(dialog,parent,setup,file,cancel,
      +[](GObject* object,GAsyncResult* result,gpointer user) {
        auto* job=static_cast<Print*>(user);GError* error=nullptr;
        if(!gtk_print_dialog_print_file_finish(GTK_PRINT_DIALOG(object),result,&error)){job->fail(error);return;}
        g_task_return_pointer(job->task,gtk_print_setup_ref(job->setup),reinterpret_cast<GDestroyNotify>(gtk_print_setup_unref));delete job;
      },this);
    g_object_unref(file);
  }
};
class PDF final : public CefPdfPrintCallback {
public:
  explicit PDF(Print* job):job_(job){}
  void OnPdfPrintFinished(const CefString&,bool success) override {
    auto* job=job_;job_=nullptr;job->rendered(success);
  }
private:
  Print* job_;
  IMPLEMENT_REFCOUNTING(PDF);
};
}
void ephy_engine_web_view_print(EphyEngineView* view,GtkPageSetup* page,GtkPrintSettings* settings,GAsyncReadyCallback cb,gpointer user) {
  auto* cancel=g_cancellable_new();
  auto* job=new Print{g_task_new(view,cancel,cb,user),gtk_print_dialog_new()};
  job->cancel=cancel;
  job->unmap=g_signal_connect(view,"unmap",G_CALLBACK(+[](GtkWidget*,Print* job){g_cancellable_cancel(job->cancel);}),job);
  if(auto* root=gtk_widget_get_root(GTK_WIDGET(view));root&&GTK_IS_WINDOW(root))job->parent=GTK_WINDOW(g_object_ref(root));
  gtk_print_dialog_set_page_setup(job->dialog,page);gtk_print_dialog_set_print_settings(job->dialog,settings);
  gtk_print_dialog_setup(job->dialog,job->parent,job->cancel,+[](GObject* object,GAsyncResult* result,gpointer user) {
    auto* job=static_cast<Print*>(user);GError* error=nullptr;
    job->setup=gtk_print_dialog_setup_finish(GTK_PRINT_DIALOG(object),result,&error);
    if(!job->setup){job->fail(error);return;}
    auto* view=EPHY_ENGINE_VIEW(g_task_get_source_object(job->task));auto browser=ephy_engine_view_browser(view);
    auto brave=ephy_engine_view_brave_page(view);
    if(!browser&&!brave){job->fail(g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CLOSED,"The tab was closed"));return;}
    char* directory=g_dir_make_tmp("gnosis-print-XXXXXX",&error);
    if(!directory){job->fail(error);return;}
    job->directory=directory;g_free(directory);job->path=job->directory+"/document.pdf";
    auto* setup=gtk_print_setup_get_page_setup(job->setup);
    if(brave) {
      brave->print_pdf(job->path,setup,job->cancel,[job](const std::string& failure) {
        if(!failure.empty() && !g_cancellable_is_cancelled(job->cancel)) {
          job->fail(g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,failure.c_str()));return;
        }
        job->rendered(failure.empty());
      });
      return;
    }
    auto* paper=gtk_page_setup_get_paper_size(setup);
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
