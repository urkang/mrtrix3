/*
    Copyright 2008 Brain Research Institute, Melbourne, Australia

    Written by J-Donald Tournier, 22/01/09.

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "app.h"
#include "icon.h"
#include "dwi/render_window.h"
#include "dwi/render_frame.h"
#include "dialog/lighting.h"
#include "dialog/file.h"

namespace MR {
  namespace DWI {

    Window::Window (const std::string& title, const Math::Matrix<float>& coefs) : 
      lighting_dialog (NULL),
      name (title), 
      current (0), 
      values (coefs)
    {
      setWindowIcon (get_icon());
      setMinimumSize (300, 300);

      QAction* open_action = new QAction ("&Open", this);
      open_action->setShortcut (tr("Ctrl+O"));
      open_action->setStatusTip (tr("Open surface plot file"));
      connect (open_action, SIGNAL (triggered()), this, SLOT (open_slot()));

      QAction* close_action = new QAction ("&Close", this);
      close_action->setShortcut (tr("Ctrl+W"));
      close_action->setStatusTip (tr("Close current surface plot file"));
      connect (close_action, SIGNAL (triggered()), this, SLOT (close_slot()));

      QAction* previous_action = new QAction ("&Previous", this);
      previous_action->setShortcut (tr("Left"));
      previous_action->setStatusTip (tr("Use values from previous row of SH coefficients matrix"));
      connect (previous_action, SIGNAL (triggered()), this, SLOT (previous_slot()));

      QAction* next_action = new QAction ("&Next", this);
      next_action->setShortcut (tr("Right"));
      next_action->setStatusTip (tr("Use values from next row of SH coefficients matrix"));
      connect (next_action, SIGNAL (triggered()), this, SLOT (next_slot()));

      QAction* previous_10_action = new QAction ("Previous (fast)", this);
      previous_10_action->setShortcut (tr("Shift+Left"));
      previous_10_action->setStatusTip (tr("Decrease current row of SH matrix by 10"));
      connect (previous_10_action, SIGNAL (triggered()), this, SLOT (previous_10_slot()));

      QAction* next_10_action = new QAction ("Next (fast)", this);
      next_10_action->setShortcut (tr("Shift+Right"));
      next_10_action->setStatusTip (tr("Increase current row of SH matrix by 10Use values from next row of SH coefficients matrix"));
      connect (next_10_action, SIGNAL (triggered()), this, SLOT (next_10_slot()));

      QAction* screenshot_action = new QAction ("Grab &Screenshot", this);
      screenshot_action->setShortcut (tr("S"));
      screenshot_action->setStatusTip (tr("Take a screenshot of the current window contents"));
      connect (screenshot_action, SIGNAL (triggered()), this, SLOT (screenshot_slot()));

      QAction* quit_action = new QAction ("&Quit", this);
      quit_action->setShortcut (tr("Ctrl+Q"));
      quit_action->setStatusTip (tr("Quit the application"));
      connect (quit_action, SIGNAL (triggered()), qApp, SLOT (quit()));

      QMenu* file_menu = menuBar()->addMenu(tr("&File"));
      file_menu->addAction (open_action);
      file_menu->addAction (close_action);
      file_menu->addSeparator();
      file_menu->addAction (previous_action);
      file_menu->addAction (next_action);
      file_menu->addSeparator();
      file_menu->addAction (previous_10_action);
      file_menu->addAction (next_10_action);
      file_menu->addSeparator();
      file_menu->addAction (screenshot_action);
      QMenu* screenshot_OS_menu = file_menu->addMenu ("&Oversampling");
      file_menu->addSeparator();
      file_menu->addAction (quit_action);

      QAction* use_lighting_action = new QAction ("&Lighting", this);
      use_lighting_action->setCheckable (true);
      use_lighting_action->setChecked (true);
      use_lighting_action->setShortcut (tr("L"));
      use_lighting_action->setStatusTip (tr("Render using lighting"));
      connect (use_lighting_action, SIGNAL (triggered(bool)), this, SLOT (use_lighting_slot(bool)));

      QAction* show_axes_action = new QAction ("Show &axes", this);
      show_axes_action->setCheckable (true);
      show_axes_action->setChecked (true);
      show_axes_action->setShortcut (tr("A"));
      show_axes_action->setStatusTip (tr("Show coordinate axes"));
      connect (show_axes_action, SIGNAL (triggered(bool)), this, SLOT (show_axes_slot(bool)));

      QAction* hide_negative_lobes_action = new QAction ("&Hide negative lobes", this);
      hide_negative_lobes_action->setCheckable (true);
      hide_negative_lobes_action->setChecked (true);
      hide_negative_lobes_action->setShortcut (tr("H"));
      hide_negative_lobes_action->setStatusTip (tr("Hide negative lobes"));
      connect (hide_negative_lobes_action, SIGNAL (triggered(bool)), this, SLOT (hide_negative_lobes_slot(bool)));

      QAction* colour_by_direction_action = new QAction ("&Colour by direction", this);
      colour_by_direction_action->setCheckable (true);
      colour_by_direction_action->setChecked (true);
      colour_by_direction_action->setShortcut (tr("C"));
      colour_by_direction_action->setStatusTip (tr("Colour surface according to direction"));
      connect (colour_by_direction_action, SIGNAL (triggered(bool)), this, SLOT (colour_by_direction_slot(bool)));

      QAction* normalise_action = new QAction ("&Normalise", this);
      normalise_action->setCheckable (true);
      normalise_action->setChecked (true);
      normalise_action->setShortcut (tr("N"));
      normalise_action->setStatusTip (tr("Normalise surface intensity"));
      connect (normalise_action, SIGNAL (triggered(bool)), this, SLOT (normalise_slot(bool)));

      QAction* advanced_lighting_action = new QAction ("A&dvanced Lighting", this);
      advanced_lighting_action->setShortcut (tr("D"));
      advanced_lighting_action->setStatusTip (tr("Modify advanced lighting settings"));
      connect (advanced_lighting_action, SIGNAL (triggered()), this, SLOT (advanced_lighting_slot()));


      QMenu* settings_menu = menuBar()->addMenu(tr("&Settings"));
      settings_menu->addAction (use_lighting_action);
      settings_menu->addAction (show_axes_action);
      settings_menu->addAction (hide_negative_lobes_action);
      settings_menu->addAction (colour_by_direction_action);
      settings_menu->addAction (normalise_action);
      settings_menu->addSeparator();
      QMenu* lmax_menu = settings_menu->addMenu (tr("&Harmonic order"));
      QMenu* lod_menu = settings_menu->addMenu (tr("Level of &detail"));
      settings_menu->addSeparator();
      settings_menu->addAction (advanced_lighting_action);

      QAction* lmax_inc_action = new QAction ("&Increase", this);
      lmax_inc_action->setShortcut (tr("PgUp"));
      lmax_inc_action->setStatusTip (tr("Increase harmonic order"));
      connect (lmax_inc_action, SIGNAL (triggered()), this, SLOT (lmax_inc_slot()));

      QAction* lmax_dec_action = new QAction ("&Decrease", this);
      lmax_dec_action->setShortcut (tr("PgDown"));
      lmax_dec_action->setStatusTip (tr("Decrease harmonic order"));
      connect (lmax_dec_action, SIGNAL (triggered()), this, SLOT (lmax_dec_slot()));

      lmax_menu->addAction (lmax_inc_action);
      lmax_menu->addAction (lmax_dec_action);
      lmax_menu->addSeparator();

      lmax_group = new QActionGroup (this);
      for (int n = 0; n < 8; n++) {
        int num = 2*(n+1);
        QString label = QString::number (num);
        QAction* lmax_action = new QAction (label, this);
        lmax_action->setCheckable (true);
        lmax_action->setData (num);
        lmax_group->addAction (lmax_action);
        lmax_menu->addAction (lmax_action);
        connect (lmax_action, SIGNAL (triggered()), this, SLOT (lmax_slot()));
      }
      lmax_group->actions()[0]->setChecked (true);


      lod_group = new QActionGroup (this);
      for (int n = 0; n < 5; n++) {
        int num = n+3;
        QString label = QString::number (num);
        QAction* lod_action = new QAction (label, this);
        lod_action->setShortcut (label);
        lod_action->setCheckable (true);
        lod_action->setData (num);
        lod_group->addAction (lod_action);
        lod_menu->addAction (lod_action);
        connect (lod_action, SIGNAL (triggered()), this, SLOT (lod_slot()));
      }
      lod_group->actions()[2]->setChecked (true);


      screenshot_OS_group = new QActionGroup (this);
      for (int n = 0; n < 4; n++) {
        int num = n+1;
        QString label = QString::number (num);
        QAction* screenshot_OS_action = new QAction (label, this);
        screenshot_OS_action->setCheckable (true);
        screenshot_OS_action->setData (num);
        screenshot_OS_group->addAction (screenshot_OS_action);
        screenshot_OS_menu->addAction (screenshot_OS_action);
      }
      screenshot_OS_group->actions()[0]->setChecked (true);

      render_frame = new RenderFrame (this);
      setCentralWidget (render_frame);

      set_values (current);
      
      render_frame->set_lmax (Math::SH::LforN(values.columns()));
      render_frame->set_normalise (true);
      render_frame->set_LOD (5);

      lmax_group->actions()[(render_frame->get_lmax()/2)-1]->setChecked (true);
      lod_group->actions()[render_frame->get_LOD()-3]->setChecked (true);
    }


    void Window::open_slot () 
    { 
      Dialog::File dialog (this, "Select SH coefficients file", false, true); 
      VAR (dialog.exec());
    }

    void Window::close_slot () { TEST; }
    void Window::use_lighting_slot (bool is_checked) { render_frame->set_use_lighting (is_checked); }
    void Window::show_axes_slot (bool is_checked) { render_frame->set_show_axes (is_checked); }
    void Window::hide_negative_lobes_slot (bool is_checked) { render_frame->set_hide_neg_lobes (is_checked); }
    void Window::colour_by_direction_slot (bool is_checked) { render_frame->set_color_by_dir (is_checked); }
    void Window::normalise_slot (bool is_checked) { render_frame->set_normalise (is_checked); }
    void Window::lmax_slot () { render_frame->set_lmax (lmax_group->checkedAction()->data().toInt()); }
    void Window::lod_slot () { render_frame->set_LOD (lod_group->checkedAction()->data().toInt()); }

    void Window::lmax_inc_slot () 
    {
      QList<QAction*> actions = lmax_group->actions();
      int index = actions.indexOf (lmax_group->checkedAction());
      if (index < 7) {
        actions[index+1]->setChecked (true);
        lmax_slot();
      }
    }


    void Window::lmax_dec_slot () 
    { 
      QList<QAction*> actions = lmax_group->actions();
      int index = actions.indexOf (lmax_group->checkedAction());
      if (index > 0) {
        actions[index-1]->setChecked (true);
        lmax_slot();
      }
    }


    void Window::previous_slot () { set_values (current-1); }
    void Window::next_slot () { set_values (current+1); }
    void Window::previous_10_slot () { set_values (current-10); }
    void Window::next_10_slot () { set_values (current+10); }


    void Window::set_values (int row)
    {
      current = row;
      if (current < 0) current = 0;
      else if (current >= int (values.rows())) current = int (values.rows())-1;

      std::vector<float> val (values.columns());
      for (size_t n = 0; n < values.columns(); n++)
        val[n] = values (current,n);
      render_frame->set (val);
      setWindowTitle (QString (std::string (name + " [ " + str(current) + " ]").c_str()));
    }

    void Window::screenshot_slot () { render_frame->screenshot (screenshot_OS_group->checkedAction()->data().toInt(), "screenshot.png"); }


    void Window::advanced_lighting_slot () 
    { 
      if (!lighting_dialog) 
        lighting_dialog = new Dialog::Lighting (this, "Advanced Lighting", *render_frame->lighting);
      lighting_dialog->show();
    }



  }
}



