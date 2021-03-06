#include <utility>
#include <QFileDialog>
#include <QStringListModel>
#include <QCompleter>
#include <QSplitter>
#include <QSettings>
#include <QClipboard>
#include <QShortcut>
#include "tracking_window.h"
#include "ui_tracking_window.h"
#include "opengl/glwidget.h"
#include "opengl/renderingtablewidget.h"
#include "region/regiontablewidget.h"
#include <QApplication>
#include <QScrollBar>
#include <QMouseEvent>
#include <QMessageBox>
#include "fib_data.hpp"
#include "manual_alignment.h"
#include "tract_report.hpp"
#include "color_bar_dialog.hpp"
#include "connectivity_matrix_dialog.h"
#include "vbc/vbc_database.h"
#include "mapping/atlas.hpp"
#include "mapping/fa_template.hpp"
#include "tracking/atlasdialog.h"
#include "libs/tracking/tracking_thread.hpp"
#include "individual_connectometry.hpp"

extern std::vector<atlas> atlas_list;
extern fa_template fa_template_imp;
extern std::vector<tracking_window*> track_windows;
QByteArray default_geo,default_state;

void tracking_window::closeEvent(QCloseEvent *event)
{
    QMainWindow::closeEvent(event);
}

QVariant tracking_window::operator[](QString name) const
{
    return renderWidget->getData(name);
}
void tracking_window::set_data(QString name, QVariant value)
{
    renderWidget->setData(name,value);
}

tracking_window::tracking_window(QWidget *parent,std::shared_ptr<fib_data> new_handle) :
        QMainWindow(parent),handle(new_handle),
        ui(new Ui::tracking_window),scene(*this),slice(new_handle),gLdock(0),renderWidget(0),glWidget2(0)

{
    fib_data& fib = *new_handle;

    odf_size = fib.dir.odf_table.size();
    odf_face_size = fib.dir.odf_faces.size();

    ui->setupUi(this);
    {
        QSettings settings;
        ui->rendering_efficiency->setCurrentIndex(settings.value("rendering_quality",1).toInt());
    }
    {
        setGeometry(10,10,800,600);

        ui->regionDockWidget->setMinimumWidth(0);
        ui->ROIdockWidget->setMinimumWidth(0);
        ui->renderingLayout->addWidget(renderWidget = new RenderingTableWidget(*this,ui->renderingWidgetHolder));
        ui->glLayout->addWidget(glWidget = new GLWidget(renderWidget->getData("anti_aliasing").toInt(),*this,renderWidget));
        ui->verticalLayout_3->addWidget(regionWidget = new RegionTableWidget(*this,ui->regionDockWidget));
        ui->track_verticalLayout->addWidget(tractWidget = new TractTableWidget(*this,ui->TractWidgetHolder));
        ui->graphicsView->setScene(&scene);
        ui->graphicsView->setCursor(Qt::CrossCursor);
        scene.statusbar = ui->statusbar;
        color_bar.reset(new color_bar_dialog(this));
    }



    // setup sliders
    {
        slice_no_update = true;
        ui->SagSlider->setRange(0,slice.geometry[0]-1);
        ui->CorSlider->setRange(0,slice.geometry[1]-1);
        ui->AxiSlider->setRange(0,slice.geometry[2]-1);
        ui->SagSlider->setValue(slice.slice_pos[0]);
        ui->CorSlider->setValue(slice.slice_pos[1]);
        ui->AxiSlider->setValue(slice.slice_pos[2]);

        ui->glSagBox->setRange(0,slice.geometry[0]-1);
        ui->glCorBox->setRange(0,slice.geometry[1]-1);
        ui->glAxiBox->setRange(0,slice.geometry[2]-1);
        ui->glSagBox->setValue(slice.slice_pos[0]);
        ui->glCorBox->setValue(slice.slice_pos[1]);
        ui->glAxiBox->setValue(slice.slice_pos[2]);

        ui->min_color->setColor(QColor(0,0,0));
        ui->max_color->setColor(QColor(255,255,255));
        ui->min_color_gl->setColor(QColor(0,0,0));
        ui->max_color_gl->setColor(QColor(255,255,255));
        v2c.two_color(ui->min_color->color().rgb(),ui->max_color->color().rgb());
        v2c_gl.two_color(ui->min_color->color().rgb(),ui->max_color->color().rgb());

        slice_no_update = false;

        for (unsigned int index = 0;index < fib.view_item.size(); ++index)
            ui->sliceViewBox->addItem(fib.view_item[index].name.c_str());
    }

    if(handle->is_qsdr)
    {
        QStringList wm,t1;
        wm << QCoreApplication::applicationDirPath() + "/mni_icbm152_wm_tal_nlin_asym_09a.nii.gz";
        t1 << QCoreApplication::applicationDirPath() + "/mni_icbm152_t1_tal_nlin_asym_09a.nii.gz";
        if(!QFileInfo(t1[0]).exists())
        {
            t1.clear();
            t1 << QDir::currentPath() + "/mni_icbm152_t1_tal_nlin_asym_09a.nii.gz";
        }

        if(!QFileInfo(wm[0]).exists())
        {
            wm.clear();
            wm << QDir::currentPath() + "/mni_icbm152_wm_tal_nlin_asym_09a.nii.gz";
        }


        if(QFileInfo(t1[0]).exists() && addSlices(t1,false,false))
            add_slice_name("T1w");
        if(QFileInfo(wm[0]).exists() && addSlices(wm,false,false))
            add_slice_name("wm");

    }

    if(!handle->is_human_data || handle->is_qsdr)
        ui->actionManual_Registration->setEnabled(false);


    {
        std::vector<std::string> index_list;
        fib.get_index_list(index_list);
        for (int index = 0; index < index_list.size(); ++index)
            {
                std::string& name = index_list[index];
                QAction* Item = new QAction(this);
                Item->setText(QString("Save %1...").arg(name.c_str()));
                Item->setData(QString(name.c_str()));
                Item->setVisible(true);
                connect(Item, SIGNAL(triggered()),tractWidget, SLOT(save_tracts_data_as()));
                ui->menuSave->addAction(Item);
            }
    }

    // opengl
    {
        connect(ui->glSagSlider,SIGNAL(valueChanged(int)),this,SLOT(glSliderValueChanged()));
        connect(ui->glCorSlider,SIGNAL(valueChanged(int)),this,SLOT(glSliderValueChanged()));
        connect(ui->glAxiSlider,SIGNAL(valueChanged(int)),this,SLOT(glSliderValueChanged()));

        connect(ui->min_value_gl,SIGNAL(valueChanged(double)),this,SLOT(update_gl()));
        connect(ui->max_value_gl,SIGNAL(valueChanged(double)),this,SLOT(update_gl()));

        connect(ui->glSagCheck,SIGNAL(stateChanged(int)),this,SLOT(update_gl()));
        connect(ui->glCorCheck,SIGNAL(stateChanged(int)),this,SLOT(update_gl()));
        connect(ui->glAxiCheck,SIGNAL(stateChanged(int)),this,SLOT(update_gl()));

        connect(ui->addSlices,SIGNAL(clicked()),this,SLOT(on_actionInsert_T1_T2_triggered()));
        connect(ui->actionAdd_surface,SIGNAL(triggered()),glWidget,SLOT(addSurface()));
        connect(ui->SliceModality,SIGNAL(currentIndexChanged(int)),this,SLOT(update_gl()));
        connect(ui->actionSave_Screen,SIGNAL(triggered()),glWidget,SLOT(catchScreen()));
        connect(ui->actionSave_3D_screen_in_high_resolution,SIGNAL(triggered()),glWidget,SLOT(catchScreen2()));
        connect(ui->actionLoad_Camera,SIGNAL(triggered()),glWidget,SLOT(loadCamera()));
        connect(ui->actionSave_Camera,SIGNAL(triggered()),glWidget,SLOT(saveCamera()));
        connect(ui->actionSave_Rotation_Images,SIGNAL(triggered()),glWidget,SLOT(saveRotationSeries()));
        connect(ui->actionSave_Rotation_Video_in_Left_Right_3D,SIGNAL(triggered()),glWidget,SLOT(saveRotationVideo2()));
        connect(ui->actionSave_Left_Right_3D_Image,SIGNAL(triggered()),glWidget,SLOT(saveLeftRight3DImage()));
        connect(ui->actionSave_3D_screen_in_3_views,SIGNAL(triggered()),glWidget,SLOT(save3ViewImage()));
    }
    // scene view
    {
        connect(ui->SagSlider,SIGNAL(valueChanged(int)),this,SLOT(SliderValueChanged()));
        connect(ui->CorSlider,SIGNAL(valueChanged(int)),this,SLOT(SliderValueChanged()));
        connect(ui->AxiSlider,SIGNAL(valueChanged(int)),this,SLOT(SliderValueChanged()));


        connect(&scene,SIGNAL(need_update()),&scene,SLOT(show_slice()));
        connect(&scene,SIGNAL(need_update()),this,SLOT(update_gl()));

        connect(ui->actionAxial_View,SIGNAL(triggered()),this,SLOT(on_AxiView_clicked()));
        connect(ui->actionCoronal_View,SIGNAL(triggered()),this,SLOT(on_CorView_clicked()));
        connect(ui->actionSagittal_view,SIGNAL(triggered()),this,SLOT(on_SagView_clicked()));


        connect(ui->actionSave_ROI_Screen,SIGNAL(triggered()),&scene,SLOT(catch_screen()));

        connect(ui->actionSave_Anisotrpy_Map_as,SIGNAL(triggered()),&scene,SLOT(save_slice_as()));


        connect(ui->sliceViewBox,SIGNAL(currentIndexChanged(int)),&scene,SLOT(show_slice()));
        connect(ui->save_roi_map,SIGNAL(clicked()),&scene,SLOT(save_slice_as()));


    }

    // regions
    {

        connect(regionWidget,SIGNAL(need_update()),&scene,SLOT(show_slice()));
        connect(regionWidget,SIGNAL(currentCellChanged(int,int,int,int)),&scene,SLOT(show_slice()));
        connect(regionWidget,SIGNAL(need_update()),this,SLOT(update_gl()));
        connect(ui->whole_brain,SIGNAL(clicked()),regionWidget,SLOT(whole_brain()));

        connect(ui->actionNewRegion,SIGNAL(triggered()),regionWidget,SLOT(new_region()));
        connect(ui->actionOpenRegion,SIGNAL(triggered()),regionWidget,SLOT(load_region()));
        connect(ui->actionLoad_From_Atlas,SIGNAL(triggered()),this,SLOT(on_addRegionFromAtlas_clicked()));
        connect(ui->actionSaveRegionAs,SIGNAL(triggered()),regionWidget,SLOT(save_region()));
        connect(ui->actionSave_All_Regions_As,SIGNAL(triggered()),regionWidget,SLOT(save_all_regions()));
        connect(ui->actionSave_All_Regions_As_Multiple_Files,SIGNAL(triggered()),regionWidget,SLOT(save_all_regions_to_dir()));
        connect(ui->actionSave_Voxel_Data_As,SIGNAL(triggered()),regionWidget,SLOT(save_region_info()));
        connect(ui->actionDeleteRegion,SIGNAL(triggered()),regionWidget,SLOT(delete_region()));
        connect(ui->actionDeleteRegionAll,SIGNAL(triggered()),regionWidget,SLOT(delete_all_region()));

        connect(ui->actionCopy_Region,SIGNAL(triggered()),regionWidget,SLOT(copy_region()));

        // actions
        connect(ui->actionUndo_Edit,SIGNAL(triggered()),regionWidget,SLOT(undo()));
        connect(ui->actionRedo_Edit,SIGNAL(triggered()),regionWidget,SLOT(redo()));
        connect(ui->actionShift_X,SIGNAL(triggered()),regionWidget,SLOT(action_shiftx()));
        connect(ui->actionShift_X_2,SIGNAL(triggered()),regionWidget,SLOT(action_shiftnx()));
        connect(ui->actionShift_Y,SIGNAL(triggered()),regionWidget,SLOT(action_shifty()));
        connect(ui->actionShift_Y_2,SIGNAL(triggered()),regionWidget,SLOT(action_shiftny()));
        connect(ui->actionShift_Z,SIGNAL(triggered()),regionWidget,SLOT(action_shiftz()));
        connect(ui->actionShift_Z_2,SIGNAL(triggered()),regionWidget,SLOT(action_shiftnz()));

        connect(ui->actionFlip_X,SIGNAL(triggered()),regionWidget,SLOT(action_flipx()));
        connect(ui->actionFlip_Y,SIGNAL(triggered()),regionWidget,SLOT(action_flipy()));
        connect(ui->actionFlip_Z,SIGNAL(triggered()),regionWidget,SLOT(action_flipz()));

        connect(ui->actionThreshold,SIGNAL(triggered()),regionWidget,SLOT(action_threshold()));



        connect(ui->actionSmoothing,SIGNAL(triggered()),regionWidget,SLOT(action_smoothing()));
        connect(ui->actionErosion,SIGNAL(triggered()),regionWidget,SLOT(action_erosion()));
        connect(ui->actionDilation,SIGNAL(triggered()),regionWidget,SLOT(action_dilation()));
        connect(ui->actionNegate,SIGNAL(triggered()),regionWidget,SLOT(action_negate()));
        connect(ui->actionDefragment,SIGNAL(triggered()),regionWidget,SLOT(action_defragment()));
        connect(ui->actionSeparate,SIGNAL(triggered()),regionWidget,SLOT(action_separate()));

        connect(ui->actionMerge_All_2,SIGNAL(triggered()),regionWidget,SLOT(merge_all()));

        connect(ui->actionCheck_all_regions,SIGNAL(triggered()),regionWidget,SLOT(check_all()));
        connect(ui->actionUnckech_all_regions,SIGNAL(triggered()),regionWidget,SLOT(uncheck_all()));

        connect(ui->actionWhole_brain_seeding,SIGNAL(triggered()),regionWidget,SLOT(whole_brain()));
        connect(ui->actionRegion_statistics,SIGNAL(triggered()),regionWidget,SLOT(show_statistics()));

        connect(ui->region_up,SIGNAL(clicked()),regionWidget,SLOT(move_up()));
        connect(ui->region_down,SIGNAL(clicked()),regionWidget,SLOT(move_down()));
    }
    // tracts
    {
        connect(ui->perform_tracking,SIGNAL(clicked()),tractWidget,SLOT(start_tracking()));
        connect(ui->stop_tracking,SIGNAL(clicked()),tractWidget,SLOT(stop_tracking()));

        connect(tractWidget,SIGNAL(need_update()),glWidget,SLOT(makeTracts()));
        connect(tractWidget,SIGNAL(need_update()),this,SLOT(update_gl()));

        connect(glWidget,SIGNAL(edited()),tractWidget,SLOT(edit_tracts()));
        connect(glWidget,SIGNAL(region_edited()),this,SLOT(update_gl()));
        connect(glWidget,SIGNAL(region_edited()),&scene,SLOT(show_slice()));


        connect(ui->actionFilter_by_ROI,SIGNAL(triggered()),tractWidget,SLOT(filter_by_roi()));

        connect(ui->actionOpenTract,SIGNAL(triggered()),tractWidget,SLOT(load_tracts()));
        connect(ui->actionOpen_Tracts_Label,SIGNAL(triggered()),tractWidget,SLOT(load_tract_label()));
        connect(ui->actionMerge_All,SIGNAL(triggered()),tractWidget,SLOT(merge_all()));
        connect(ui->actionMerge_Tracts_by_Name,SIGNAL(triggered()),tractWidget,SLOT(merge_track_by_name()));
        connect(ui->actionCopyTrack,SIGNAL(triggered()),tractWidget,SLOT(copy_track()));
        connect(ui->actionSort_Tracts_By_Names,SIGNAL(triggered()),tractWidget,SLOT(sort_track_by_name()));



        connect(ui->actionCheck_all_tracts,SIGNAL(triggered()),tractWidget,SLOT(check_all()));
        connect(ui->actionUncheck_all_tracts,SIGNAL(triggered()),tractWidget,SLOT(uncheck_all()));


        connect(ui->actionDeleteTract,SIGNAL(triggered()),tractWidget,SLOT(delete_tract()));
        connect(ui->actionDeleteTractAll,SIGNAL(triggered()),tractWidget,SLOT(delete_all_tract()));
        connect(ui->actionDelete_By_Length,SIGNAL(triggered()),tractWidget,SLOT(delete_by_length()));
        connect(ui->actionRemove_Repeated_Tracks,SIGNAL(triggered()),tractWidget,SLOT(delete_repeated()));
        connect(ui->actionSeparate_Deleted,SIGNAL(triggered()),tractWidget,SLOT(separate_deleted_track()));


        connect(ui->actionOpen_Colors,SIGNAL(triggered()),tractWidget,SLOT(load_tracts_color()));
        connect(ui->actionOpen_Tract_Property,SIGNAL(triggered()),tractWidget,SLOT(load_tracts_value()));
        connect(ui->actionSave_Tracts_Colors_As,SIGNAL(triggered()),tractWidget,SLOT(save_tracts_color_as()));

        connect(ui->actionUndo,SIGNAL(triggered()),tractWidget,SLOT(undo_tracts()));
        connect(ui->actionRedo,SIGNAL(triggered()),tractWidget,SLOT(redo_tracts()));
        connect(ui->actionTrim,SIGNAL(triggered()),tractWidget,SLOT(trim_tracts()));


        connect(ui->actionSet_Color,SIGNAL(triggered()),tractWidget,SLOT(set_color()));

        connect(ui->actionHierarchical,SIGNAL(triggered()),tractWidget,SLOT(clustering_hie()));
        connect(ui->actionOpen_Cluster_Labels,SIGNAL(triggered()),tractWidget,SLOT(open_cluster_label()));

        //setup menu
        connect(ui->actionSaveTractAs,SIGNAL(triggered()),tractWidget,SLOT(save_tracts_as()));
        connect(ui->actionSave_VMRL,SIGNAL(triggered()),tractWidget,SLOT(save_vrml_as()));
        connect(ui->actionSave_All_Tracts_As,SIGNAL(triggered()),tractWidget,SLOT(save_all_tracts_as()));
        connect(ui->actionSave_All_Tracts_As_Multiple_Files,SIGNAL(triggered()),tractWidget,SLOT(save_all_tracts_to_dir()));
        connect(ui->actionSave_End_Points_As,SIGNAL(triggered()),tractWidget,SLOT(save_end_point_as()));
        connect(ui->actionSave_Enpoints_In_MNI_Space,SIGNAL(triggered()),tractWidget,SLOT(save_end_point_in_mni()));
        connect(ui->actionSave_Profile,SIGNAL(triggered()),tractWidget,SLOT(save_profile()));
        connect(ui->actionDeep_Learning_Train,SIGNAL(triggered()),tractWidget,SLOT(deep_learning_train()));
        connect(ui->actionStatistics,SIGNAL(triggered()),tractWidget,SLOT(show_tracts_statistics()));
        connect(ui->actionRecognize_Current_Tract,SIGNAL(triggered()),tractWidget,SLOT(recog_tracks()));

        connect(ui->track_up,SIGNAL(clicked()),tractWidget,SLOT(move_up()));
        connect(ui->track_down,SIGNAL(clicked()),tractWidget,SLOT(move_down()));

    }

    // slice
    {
        connect(ui->actionLoad_Color_Map,SIGNAL(triggered()),this,SLOT(on_load_color_map_gl_released()));
    }

    // recall the setting
    {

        QSettings settings;
        if(!default_geo.size())
            default_geo = saveGeometry();
        if(!default_state.size())
            default_state = saveState();
        restoreGeometry(settings.value("geometry").toByteArray());
        restoreState(settings.value("state").toByteArray());
        ui->TractWidgetHolder->show();
        ui->renderingWidgetHolder->show();
        ui->ROIdockWidget->show();
        ui->regionDockWidget->show();
        ui->show_r->setChecked((*this)["roi_label"].toBool());
        ui->show_position->setChecked((*this)["roi_position"].toBool());
        ui->show_fiber->setChecked((*this)["roi_fiber"].toBool());
    }

    if(handle->is_human_data)
    {
        QStringList items;
        for(int i = 0;i < atlas_list.size();++i)
        {
            const std::vector<std::string>& label = atlas_list[i].get_list();
            for(auto str : label)
                items << QString(str.c_str()) + ":" + atlas_list[i].name.c_str();
        }
        ui->search_atlas->setList(items);
        connect(ui->search_atlas,SIGNAL(selected()),this,SLOT(add_roi_from_atlas()));
    }

    {
        slice_no_update = false;
    }

    // setup fa threshold
    {
        initialize_tracking_index(0);
        set_data("step_size",fib.vs[0]/2.0);
    }

    report(handle->report.c_str());
    ui->sliceViewBox->setCurrentIndex(0);
    on_SliceModality_currentIndexChanged(ui->SliceModality->count()-1);


    if(handle->dim[0] > 80)
        ui->zoom_3d->setValue(80.0/(float)std::max<int>(std::max<int>(handle->dim[0],handle->dim[1]),handle->dim[2]));

    qApp->installEventFilter(this);
    #ifdef __APPLE__ // fix Mac shortcut problem
    foreach (QAction *a, ui->menu_Edit->actions()) {
        QObject::connect(new QShortcut(a->shortcut(), a->parentWidget()),
                         SIGNAL(activated()), a, SLOT(trigger()));
    }
    #endif
}

tracking_window::~tracking_window()
{
    for(int i = 0;i < track_windows.size();++i)
        if(track_windows[i] == this)
        {
            track_windows.erase(track_windows.begin()+i);
            break;
        }
    qApp->removeEventFilter(this);
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("state", saveState());
    settings.setValue("rendering_quality",ui->rendering_efficiency->currentIndex());
    tractWidget->delete_all_tract();
    delete ui;
    //std::cout << __FUNCTION__ << " " << __FILE__ << std::endl;
}
void tracking_window::report(QString string)
{
    string += " The analysis was conducted using DSI Studio (http://dsi-studio.labsolver.org).";
    ui->text_report->setText(string);
}
bool tracking_window::command(QString cmd,QString param,QString param2)
{
    if(glWidget->command(cmd,param,param2) ||
       scene.command(cmd,param,param2) ||
       tractWidget->command(cmd,param,param2))
        return true;

    if(cmd == "set_roi_view_index")
    {
        bool okay = true;
        int index = param.toInt(&okay);
        if(okay)
        {
            ui->sliceViewBox->setCurrentIndex(index);
            return true;
        }
        index = ui->sliceViewBox->findText(param);
        if(index == -1)
        {
            std::cout << "Cannot find index:" << param.toStdString() << std::endl;
            return false;
        }
        ui->sliceViewBox->setCurrentIndex(index);
        return true;
    }
    if(cmd == "set_param")
    {
        renderWidget->setData(param,param2);
        return true;
    }

    if(cmd == "add_slice")
    {
        if(!addSlices(QStringList() << param,renderWidget->getData("slice_smoothing").toBool(),true))
            return true;
        add_slice_name(other_slices.back()->name.c_str());
        std::cout << "register image to the DWI space" << std::endl;
        if(other_slices.back()->thread.get())
            other_slices.back()->thread->wait();
        other_slices.back()->update();
        update_gl();
        return true;
    }
    return false;
}

void tracking_window::initialize_tracking_index(int index)
{
    QStringList tracking_index_list;
    for(int index = 0;index < handle->dir.index_name.size();++index)
        tracking_index_list.push_back(handle->dir.index_name[index].c_str());
    renderWidget->setList("tracking_index",tracking_index_list);
    set_data("tracking_index",index);
    on_tracking_index_currentIndexChanged(index);
    scene.center();
}
bool tracking_window::eventFilter(QObject *obj, QEvent *event)
{
    bool has_info = false;
    image::vector<3,float> pos;
    if (event->type() == QEvent::MouseMove)
    {
        if (obj == glWidget)
        {
            has_info = glWidget->get_mouse_pos(static_cast<QMouseEvent*>(event),pos);
        }
        if (obj->parent() == ui->graphicsView)
        {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
            QPointF point = ui->graphicsView->mapToScene(mouseEvent->pos().x(),mouseEvent->pos().y());
            has_info = scene.get_location(point.x(),point.y(),pos);
        }
        // for connectivity matrix
        if(connectivity_matrix.get() && connectivity_matrix->is_graphic_view(obj->parent()))
            connectivity_matrix->mouse_move(static_cast<QMouseEvent*>(event));

    }
    if(!has_info)
        return false;

    QString status;
    status = QString("(%1,%2,%3) ").arg(std::round(pos[0]*10.0)/10.0)
            .arg(std::round(pos[1]*10.0)/10.0)
            .arg(std::round(pos[2]*10.0)/10.0);

    if(handle->is_qsdr || handle->has_reg())
    {
        image::vector<3,float> mni(pos);
        handle->subject2mni(mni);
        status += QString("MNI(%1,%2,%3) ")
                .arg(std::round(mni[0]*10.0)/10.0)
                .arg(std::round(mni[1]*10.0)/10.0)
                .arg(std::round(mni[2]*10.0)/10.0);
    }
    status += " ";
    std::vector<float> data;
    pos.round();
    handle->get_voxel_information(pos[0],pos[1],pos[2], data);
    for(unsigned int index = 0,data_index = 0;index < handle->view_item.size() && data_index < data.size();++index)
        if(handle->view_item[index].name != "color")
        {
            status += handle->view_item[index].name.c_str();
            status += QString("=%1 ").arg(data[data_index]);
            ++data_index;
        }
    ui->statusbar->showMessage(status);
    return false;
}

void tracking_window::set_tracking_param(ThreadData& tracking_thread)
{
    tracking_thread.param.threshold = renderWidget->getData("fa_threshold").toFloat();
    tracking_thread.param.cull_cos_angle = std::cos(renderWidget->getData("turning_angle").toDouble() * 3.1415926 / 180.0);
    tracking_thread.param.step_size = renderWidget->getData("step_size").toDouble();
    tracking_thread.param.smooth_fraction = renderWidget->getData("smoothing").toDouble();
    tracking_thread.param.min_points_count3 = 3.0*renderWidget->getData("min_length").toDouble()/renderWidget->getData("step_size").toDouble();
    if(tracking_thread.param.min_points_count3 < 6)
        tracking_thread.param.min_points_count3 = 6;
    tracking_thread.param.max_points_count3 =
            std::max<unsigned int>(tracking_thread.param.min_points_count3,
                                   3.0*renderWidget->getData("max_length").toDouble()/renderWidget->getData("step_size").toDouble());

    tracking_thread.tracking_method = renderWidget->getData("tracking_method").toInt();
    tracking_thread.initial_direction = renderWidget->getData("initial_direction").toInt();
    tracking_thread.interpolation_strategy = renderWidget->getData("interpolation").toInt();
    tracking_thread.stop_by_tract = renderWidget->getData("tracking_plan").toInt();
    tracking_thread.center_seed = renderWidget->getData("seed_plan").toInt();
    tracking_thread.check_ending = renderWidget->getData("check_ending").toInt();
}

void tracking_window::SliderValueChanged(void)
{
    if(!slice_no_update && slice.set_slice_pos(
            ui->SagSlider->value(),
            ui->CorSlider->value(),
            ui->AxiSlider->value()))
    {
        if(renderWidget->getData("roi_layout").toInt() <= 1)
            scene.show_slice();
        if(glWidget->current_visible_slide == 0)
            update_gl();
        else
        {
            image::vector<3,float> p(ui->SagSlider->value(),ui->CorSlider->value(),ui->AxiSlider->value());
            p.to(other_slices[glWidget->current_visible_slide-1]->invT);
            p.round();
            if(other_slices[glWidget->current_visible_slide-1]->set_slice_pos(p[0],p[1],p[2]))
                update_gl();
        }
    }



}
void tracking_window::glSliderValueChanged(void)
{
    if(!glWidget->current_visible_slide)
        return;
    SliceModel& cur_slice =
                *other_slices[glWidget->current_visible_slide-1];
    if(!slice_no_update && cur_slice.set_slice_pos(
                ui->glSagSlider->value(),
                ui->glCorSlider->value(),
                ui->glAxiSlider->value()))
    {
        image::vector<3,float> p(cur_slice.slice_pos[0],cur_slice.slice_pos[1],cur_slice.slice_pos[2]);
        p.to(other_slices[glWidget->current_visible_slide-1]->transform);
        p.round();
        ui->SagSlider->setValue(p[0]);
        ui->CorSlider->setValue(p[1]);
        ui->AxiSlider->setValue(p[2]);
        scene.show_slice();
        update_gl();
    }
}

void tracking_window::on_AxiView_clicked()
{
    glWidget->set_view(2);
    update_gl();
    slice.cur_dim = 2;
    if(renderWidget->getData("roi_layout").toInt() == 0)
        scene.show_slice();
    scene.setFocus();

}

void tracking_window::on_CorView_clicked()
{
    glWidget->set_view(1);
    update_gl();
    slice.cur_dim = 1;
    if(renderWidget->getData("roi_layout").toInt() == 0)
        scene.show_slice();
    scene.setFocus();

}

void tracking_window::on_SagView_clicked()
{
    glWidget->set_view(0);
    update_gl();
    slice.cur_dim = 0;
    if(renderWidget->getData("roi_layout").toInt() == 0)
        scene.show_slice();
    scene.setFocus();
}

void tracking_window::on_tool0_pressed()
{
    scene.sel_mode = 0;
    scene.setFocus();

}

void tracking_window::on_tool1_pressed()
{
    scene.sel_mode = 1;
    scene.setFocus();

}

void tracking_window::on_tool2_pressed()
{
    scene.sel_mode = 2;
    scene.setFocus();

}

void tracking_window::on_tool3_pressed()
{
    scene.sel_mode = 3;
    scene.setFocus();
}

void tracking_window::on_tool4_clicked()
{
    scene.sel_mode = 4;
    scene.setFocus();
}

void tracking_window::on_tool5_pressed()
{
    scene.sel_mode = 5;
    scene.setFocus();

}
void tracking_window::on_tool6_pressed()
{
    scene.sel_mode = 6;
    scene.show_slice();
    scene.setFocus();

}

void tracking_window::on_sliceViewBox_currentIndexChanged(int index)
{
    ui->actionSave_Anisotrpy_Map_as->setText(QString("Save ") +
                                             ui->sliceViewBox->currentText()+" volume as...");
    ui->actionSave_Anisotrpy_Map_as->setEnabled(ui->sliceViewBox->currentText() != "color");
    slice.set_view_name(ui->sliceViewBox->currentText().toLocal8Bit().begin());
    std::pair<float,float> range = handle->get_value_range(ui->sliceViewBox->currentText().toLocal8Bit().begin());
    float r = std::fabs(range.first-range.second);
    if(r == 0.0)
        r = 1;
    float step = r/20.0;
    ui->min_value->setMinimum(range.first-r);
    ui->min_value->setMaximum(range.second+r);
    ui->min_value->setSingleStep(step);
    ui->min_value->setValue(range.first);
    ui->max_value->setMinimum(range.first-r);
    ui->max_value->setMaximum(range.second+r);
    ui->max_value->setSingleStep(step);
    ui->max_value->setValue(range.second);
    v2c.set_range(range.first,range.second);

    if(glWidget->current_visible_slide == 0) // Show diffusion
    {
        ui->min_value_gl->setMinimum(range.first-r);
        ui->min_value_gl->setMaximum(range.second+r);
        ui->min_value_gl->setSingleStep(step);
        ui->min_value_gl->setValue(range.first);
        ui->max_value_gl->setMinimum(range.first-r);
        ui->max_value_gl->setMaximum(range.second+r);
        ui->max_value_gl->setSingleStep(step);
        ui->max_value_gl->setValue(range.second);
        v2c_gl.set_range(range.first,range.second);
    }
}

void tracking_window::on_actionSelect_Tracts_triggered()
{
    glWidget->setCursor(Qt::CrossCursor);
    glWidget->editing_option = 1;
    tractWidget->edit_option = 1;

}

void tracking_window::on_actionDelete_triggered()
{
    glWidget->setCursor(Qt::CrossCursor);
    glWidget->editing_option = 1;
    tractWidget->edit_option = 2;
}

void tracking_window::on_actionCut_triggered()
{
    glWidget->setCursor(Qt::CrossCursor);
    glWidget->editing_option = 1;
    tractWidget->edit_option = 3;
}


void tracking_window::on_actionPaint_triggered()
{
    glWidget->setCursor(Qt::CrossCursor);
    glWidget->editing_option = 1;
    tractWidget->edit_option = 4;
}

void tracking_window::on_actionMove_Object_triggered()
{
    glWidget->setCursor(Qt::CrossCursor);
    glWidget->editing_option = 2;
}


void tracking_window::on_glSagView_clicked()
{
    glWidget->set_view(0);
    update_gl();
    glWidget->setFocus();
}

void tracking_window::on_glCorView_clicked()
{
    glWidget->set_view(1);
    update_gl();
    glWidget->setFocus();
}

void tracking_window::on_glAxiView_clicked()
{
    glWidget->set_view(2);
    update_gl();
    glWidget->setFocus();
}

void tracking_window::on_SliceModality_currentIndexChanged(int index)
{
    glWidget->current_visible_slide = index;
    if(glWidget2)
        glWidget2->current_visible_slide = index;
    slice_no_update = true;

    {
        std::pair<float,float> range;
        if(index)
            range = other_slices[glWidget->current_visible_slide-1]->get_value_range();
        else
            range =  handle->get_value_range(ui->sliceViewBox->currentText().toLocal8Bit().begin());
        float r = range.second-range.first;
        if(r == 0.0)
            r = 1;
        float step = r/20.0;

        ui->min_value_gl->setMinimum(range.first-r);
        ui->min_value_gl->setMaximum(range.second+r);
        ui->min_value_gl->setSingleStep(step);
        ui->min_value_gl->setValue(range.first);
        ui->max_value_gl->setMinimum(range.first-r);
        ui->max_value_gl->setMaximum(range.second+r);
        ui->max_value_gl->setSingleStep(step);
        ui->max_value_gl->setValue(range.second);
        v2c_gl.set_range(range.first,range.second);
    }

    if(index)
    {
        disconnect(ui->sliceViewBox,SIGNAL(currentIndexChanged(int)),this,SLOT(update_gl()));
        disconnect(ui->glSagSlider,SIGNAL(valueChanged(int)),ui->SagSlider,SLOT(setValue(int)));
        disconnect(ui->glCorSlider,SIGNAL(valueChanged(int)),ui->CorSlider,SLOT(setValue(int)));
        disconnect(ui->glAxiSlider,SIGNAL(valueChanged(int)),ui->AxiSlider,SLOT(setValue(int)));
        disconnect(ui->SagSlider,SIGNAL(valueChanged(int)),ui->glSagSlider,SLOT(setValue(int)));
        disconnect(ui->CorSlider,SIGNAL(valueChanged(int)),ui->glCorSlider,SLOT(setValue(int)));
        disconnect(ui->AxiSlider,SIGNAL(valueChanged(int)),ui->glAxiSlider,SLOT(setValue(int)));

        SliceModel& cur_slice =
                *other_slices[glWidget->current_visible_slide-1];

        ui->glSagSlider->setRange(0,cur_slice.geometry[0]-1);
        ui->glCorSlider->setRange(0,cur_slice.geometry[1]-1);
        ui->glAxiSlider->setRange(0,cur_slice.geometry[2]-1);
        ui->glSagBox->setRange(0,cur_slice.geometry[0]-1);
        ui->glCorBox->setRange(0,cur_slice.geometry[1]-1);
        ui->glAxiBox->setRange(0,cur_slice.geometry[2]-1);
        ui->glSagSlider->setValue(cur_slice.slice_pos[0]);
        ui->glCorSlider->setValue(cur_slice.slice_pos[1]);
        ui->glAxiSlider->setValue(cur_slice.slice_pos[2]);
        ui->glSagBox->setValue(ui->glSagSlider->value());
        ui->glCorBox->setValue(ui->glCorSlider->value());
        ui->glAxiBox->setValue(ui->glAxiSlider->value());
    }
    else
    //diffusion
    {
        ui->glSagSlider->setRange(0,slice.geometry[0]-1);
        ui->glCorSlider->setRange(0,slice.geometry[1]-1);
        ui->glAxiSlider->setRange(0,slice.geometry[2]-1);
        ui->glSagBox->setRange(0,slice.geometry[0]-1);
        ui->glCorBox->setRange(0,slice.geometry[1]-1);
        ui->glAxiBox->setRange(0,slice.geometry[2]-1);

        ui->glSagSlider->setValue(ui->SagSlider->value());
        ui->glCorSlider->setValue(ui->CorSlider->value());
        ui->glAxiSlider->setValue(ui->AxiSlider->value());
        ui->glSagBox->setValue(ui->glSagSlider->value());
        ui->glCorBox->setValue(ui->glCorSlider->value());
        ui->glAxiBox->setValue(ui->glAxiSlider->value());

        connect(ui->sliceViewBox,SIGNAL(currentIndexChanged(int)),this,SLOT(update_gl()));
        connect(ui->glSagSlider,SIGNAL(valueChanged(int)),ui->SagSlider,SLOT(setValue(int)));
        connect(ui->glCorSlider,SIGNAL(valueChanged(int)),ui->CorSlider,SLOT(setValue(int)));
        connect(ui->glAxiSlider,SIGNAL(valueChanged(int)),ui->AxiSlider,SLOT(setValue(int)));
        connect(ui->SagSlider,SIGNAL(valueChanged(int)),ui->glSagSlider,SLOT(setValue(int)));
        connect(ui->CorSlider,SIGNAL(valueChanged(int)),ui->glCorSlider,SLOT(setValue(int)));
        connect(ui->AxiSlider,SIGNAL(valueChanged(int)),ui->glAxiSlider,SLOT(setValue(int)));

        glWidget->slice_pos[0] = glWidget->slice_pos[1] = glWidget->slice_pos[2] = -1;
        if(glWidget2)
            glWidget2->slice_pos[0] = glWidget2->slice_pos[1] = glWidget2->slice_pos[2] = -1;


        ui->sliceViewBox->setCurrentIndex(0);
    }
    slice_no_update = false;

}

void tracking_window::on_actionEndpoints_to_seeding_triggered()
{
    std::vector<image::vector<3,short> >points;

    if(tractWidget->tract_models.empty())
        return;
    tractWidget->tract_models[tractWidget->currentRow()]->get_end_points(points);
    regionWidget->add_region(
            tractWidget->item(tractWidget->currentRow(),0)->text()+
            QString(" end points"),roi_id);
    regionWidget->add_points(points,false);
    scene.show_slice();
    update_gl();
}

void tracking_window::on_actionTracts_to_seeds_triggered()
{
    std::vector<image::vector<3,short> >points;
    if(tractWidget->tract_models.empty())
        return;
    tractWidget->tract_models[tractWidget->currentRow()]->get_tract_points(points);
    regionWidget->add_region(
            tractWidget->item(tractWidget->currentRow(),0)->text(),roi_id);
    regionWidget->add_points(points,false);
    scene.show_slice();
    update_gl();
}

void tracking_window::add_slice_name(QString name)
{
    ui->SliceModality->addItem(name);
    ui->sliceViewBox->addItem(name);
    handle->view_item.push_back(handle->view_item[0]);
    handle->view_item.back().name = name.toLocal8Bit().begin();
    handle->view_item.back().image_data = image::make_image(other_slices.back()->roi_image_buf,
                                                            other_slices.back()->roi_image.geometry());
    handle->view_item.back().set_scale(
                other_slices.back()->source_images.begin(),
                other_slices.back()->source_images.end());
    ui->SliceModality->setCurrentIndex(ui->SliceModality->count()-1);
    ui->sliceViewBox->setCurrentIndex(ui->sliceViewBox->count()-1);
}

void tracking_window::on_actionInsert_T1_T2_triggered()
{
    QStringList filenames = QFileDialog::getOpenFileNames(
        this,"Open Images files",QFileInfo(windowTitle()).absolutePath(),"Image files (*.dcm *.hdr *.nii *nii.gz 2dseq);;All files (*)" );
    if( filenames.isEmpty())
        return;
    if(addSlices(filenames,renderWidget->getData("slice_smoothing").toBool(),false))
        add_slice_name(other_slices.back()->name.c_str());
}


bool ask_TDI_options(int& rec,int& rec2)
{
    QMessageBox msgBox;
    msgBox.setText("Export directional color ? (BMP format only)");
    msgBox.setInformativeText("If grayscale or NIFTI format is preferred, select No.");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No| QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Yes);
    rec = msgBox.exec();
    if(rec == QMessageBox::Cancel)
        return false;
    msgBox.setText("Export whole tracts or end points?");
    msgBox.setInformativeText("Yes: whole tracts, No: end points ");
    rec2 = msgBox.exec();
    if(rec2 == QMessageBox::Cancel)
        return false;
    return true;

}
void tracking_window::on_actionTDI_Diffusion_Space_triggered()
{
    image::matrix<4,4,float> tr;
    tr.identity();
    int rec,rec2;
    if(!ask_TDI_options(rec,rec2))
        return;
    tractWidget->export_tract_density(slice.geometry,slice.voxel_size,tr,rec == QMessageBox::Yes,rec2 != QMessageBox::Yes);
}


void tracking_window::on_actionTDI_Subvoxel_Diffusion_Space_triggered()
{
    image::matrix<4,4,float> tr;
    tr.identity();
    tr[0] = tr[5] = tr[10] = 4.0;
    image::geometry<3> new_geo(slice.geometry[0]*4,slice.geometry[1]*4,slice.geometry[2]*4);
    image::vector<3,float> new_vs(slice.voxel_size);
    new_vs /= 4.0;
    int rec,rec2;
    if(!ask_TDI_options(rec,rec2))
        return;
    tractWidget->export_tract_density(new_geo,new_vs,tr,rec == QMessageBox::Yes,rec2 != QMessageBox::Yes);
}

void tracking_window::on_actionTDI_Import_Slice_Space_triggered()
{
    image::matrix<4,4,float> tr;
    image::geometry<3> geo;
    image::vector<3,float> vs;
    glWidget->get_current_slice_transformation(geo,vs,tr);
    int rec,rec2;
    if(!ask_TDI_options(rec,rec2))
        return;
    tractWidget->export_tract_density(geo,vs,tr,rec == QMessageBox::Yes,rec2 != QMessageBox::Yes);
}


void tracking_window::on_actionSave_Tracts_in_Current_Mapping_triggered()
{
    image::matrix<4,4,float> tr;
    image::geometry<3> geo;
    image::vector<3,float> vs;
    glWidget->get_current_slice_transformation(geo,vs,tr);
    tractWidget->saveTransformedTracts(&*tr.begin());
}
void tracking_window::on_actionSave_Endpoints_in_Current_Mapping_triggered()
{
    image::matrix<4,4,float> tr;
    image::geometry<3> geo;
    image::vector<3,float> vs;
    glWidget->get_current_slice_transformation(geo,vs,tr);
    tractWidget->saveTransformedEndpoints(&*tr.begin());
}


void tracking_window::on_actionRestore_window_layout_triggered()
{
    restoreGeometry(default_geo);
    restoreState(default_state);
}



void tracking_window::on_tracking_index_currentIndexChanged(int index)
{
    if(index < 0)
        return;
    handle->dir.set_tracking_index(index);
    if(handle->dir.index_name[index] == "<%" ||
        handle->dir.index_name[index] == ">%")
    {
        // percentile threshold
        renderWidget->setMinMax("fa_threshold",0.0,1.0,0.05);
        set_data("fa_threshold",0.95);
        scene.show_slice();
        return;
    }
    if(handle->dir.index_name[index] == "inc" ||
        handle->dir.index_name[index] == "dec")
    {
        // percentile threshold
        renderWidget->setMinMax("fa_threshold",0.0,1.0,0.05);
        set_data("fa_threshold",0.05);
        scene.show_slice();
        return;
    }
    float max_value = *std::max_element(handle->dir.fa[0],handle->dir.fa[0]+handle->dim.size());
    renderWidget->setMinMax("fa_threshold",0.0,max_value*1.1,max_value/50.0);
    set_data("fa_threshold",0.6*image::segmentation::otsu_threshold(image::make_image(handle->dir.fa[0],handle->dim)));
    scene.show_slice();
}


void tracking_window::on_deleteSlice_clicked()
{
    if(ui->SliceModality->currentIndex() == 0)
        return;
    int index = ui->SliceModality->currentIndex();
    unsigned int view_item_index = handle->view_item.size()-other_slices.size()+index-1;
    if(ui->sliceViewBox->currentIndex() == view_item_index)
        ui->sliceViewBox->setCurrentIndex(0);
    ui->sliceViewBox->removeItem(view_item_index);
    handle->view_item.erase(handle->view_item.begin()+view_item_index);
    ui->SliceModality->setCurrentIndex(0);
    other_slices.pop_back();
    ui->SliceModality->removeItem(index);
}


void tracking_window::on_actionSave_Tracts_in_MNI_space_triggered()
{
    if(!handle->can_map_to_mni())
    {
        QMessageBox::information(this,"Error","MNI normalization is not supported for the current image resolution",0);
        return;
    }
    if(handle->is_qsdr)
        tractWidget->saveTransformedTracts(&*(handle->trans_to_mni.begin()));
        tractWidget->saveTransformedTracts(0);
}




void tracking_window::keyPressEvent ( QKeyEvent * event )
{
    switch(event->key())
    {
        case Qt::Key_Q:
            ui->SagSlider->setValue(ui->SagSlider->value()+1);
            event->accept();
            break;
        case Qt::Key_A:
            ui->SagSlider->setValue(ui->SagSlider->value()-1);
            event->accept();
            break;
        case Qt::Key_W:
            ui->CorSlider->setValue(ui->CorSlider->value()+1);
            event->accept();
            break;
        case Qt::Key_S:
            ui->CorSlider->setValue(ui->CorSlider->value()-1);
            event->accept();
            break;
        case Qt::Key_E:
            ui->AxiSlider->setValue(ui->AxiSlider->value()+1);
            event->accept();
            break;
        case Qt::Key_D:
            ui->AxiSlider->setValue(ui->AxiSlider->value()-1);
            event->accept();
            break;

        case Qt::Key_Left:
            glWidget->move_by(-1,0);
            break;
        case Qt::Key_Right:
            glWidget->move_by(1,0);
            break;
        case Qt::Key_Up:
            glWidget->move_by(0,-1);
            break;
        case Qt::Key_Down:
            glWidget->move_by(0,1);
            break;
    }

    if(event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9)
    {
        QSettings settings;
        event->accept();
        int key_num =  event->key()-Qt::Key_1;
        char key_str[3] = "F1";
        key_str[1] += key_num;
        if(event->modifiers() & Qt::AltModifier)
        {
            std::ostringstream out;
            out << ui->glSagSlider->value() << " "
                << ui->glCorSlider->value() << " "
                << ui->glAxiSlider->value() << " ";
            std::copy(glWidget->transformation_matrix.begin(),glWidget->transformation_matrix.end(),std::ostream_iterator<float>(out," "));
            settings.setValue(key_str,QString(out.str().c_str()));
        }
        else
        {
            QString value = settings.value(key_str,"").toString();
            if(value == "")
                return;
            std::istringstream in(value.toLocal8Bit().begin());
            int sag,cor,axi;
            in >> sag >> cor >> axi;
            std::vector<float> tran((std::istream_iterator<float>(in)),(std::istream_iterator<float>()));
            if(tran.size() != 16)
                return;
            std::copy(tran.begin(),tran.begin()+16,glWidget->transformation_matrix.begin());
            ui->glSagSlider->setValue(sag);
            ui->glCorSlider->setValue(cor);
            ui->glAxiSlider->setValue(axi);
            update_gl();
        }
    }
    if(event->isAccepted())
        return;
    QWidget::keyPressEvent(event);

}



void tracking_window::on_actionManual_Registration_triggered()
{
    image::basic_image<float,3> from = slice.source_images;
    image::filter::gaussian(from);
    from -= image::segmentation::otsu_threshold(from);
    image::lower_threshold(from,0.0);
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
                                   from,handle->vs,
                                   fa_template_imp.I,fa_template_imp.vs,
                                   image::reg::affine,image::reg::reg_cost_type::corr));

    manual->timer->start();
    if(manual->exec() != QDialog::Accepted)
        return;
    handle->thread.clear();
    handle->reg = manual->data;
}


void tracking_window::on_actionTract_Analysis_Report_triggered()
{
    if(!tact_report_imp.get())
        tact_report_imp.reset(new tract_report(this));
    tact_report_imp->show();
    tact_report_imp->on_refresh_report_clicked();
}

void tracking_window::on_actionConnectivity_matrix_triggered()
{
    if(!tractWidget->tract_models.size())
    {
        QMessageBox::information(this,"DSI Studio","Run fiber tracking first",0);
        return;
    }
    if(atlas_list.empty())
        QMessageBox::information(0,"Error",QString("DSI Studio cannot find atlas files in ")+QCoreApplication::applicationDirPath()+ "/atlas",0);
    std::ostringstream out;
    if(tractWidget->currentRow() < tractWidget->tract_models.size())
        out << tractWidget->tract_models[tractWidget->currentRow()]->report.c_str() << std::endl;
    connectivity_matrix.reset(new connectivity_matrix_dialog(this,out.str().c_str()));
    connectivity_matrix->show();
}


void tracking_window::on_zoom_3d_valueChanged(double arg1)
{
    glWidget->command("set_zoom",QString::number(ui->zoom_3d->value()));
}

void tracking_window::restore_3D_window()
{
    ui->centralLayout->addWidget(ui->main_widget);
    gLdock.reset(0);
}

void tracking_window::float3dwindow(int w,int h)
{
    if(!gLdock.get())
    {
        gLdock.reset(new QGLDockWidget(this));
        gLdock->setWindowTitle("3D Window");
        gLdock->setAllowedAreas(Qt::NoDockWidgetArea);
        gLdock->setWidget(ui->main_widget);
    }
    gLdock->setFloating(true);
    gLdock->show();
    gLdock->resize(w,h+44);
    connect(gLdock.get(),SIGNAL(closedSignal()),this,SLOT(restore_3D_window()));
}

void tracking_window::on_actionFloat_3D_window_triggered()
{
    if(gLdock.get())
    {
        if(gLdock->isFullScreen())
            gLdock->showNormal();
        else
            gLdock->showFullScreen();
    }
    else
        float3dwindow(ui->main_widget->width(),ui->main_widget->height());
}

void tracking_window::on_actionSave_tracking_parameters_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                           this,
                           "Save INI files",QFileInfo(windowTitle()).baseName()+".ini","Setting file (*.ini);;All files (*)");
    if (filename.isEmpty())
        return;
    QSettings s(filename, QSettings::IniFormat);
    QStringList param_list = renderWidget->getChildren("Tracking");
    for(unsigned int index = 0;index < param_list.size();++index)
        s.setValue(param_list[index],renderWidget->getData(param_list[index]));

}

void tracking_window::on_actionLoad_tracking_parameters_triggered()
{
    QString filename = QFileDialog::getOpenFileName(
                           this,"Open INI files",QFileInfo(windowTitle()).absolutePath(),"Setting file (*.ini);;All files (*)");
    if (filename.isEmpty())
        return;
    QSettings s(filename, QSettings::IniFormat);
    QStringList param_list = renderWidget->getChildren("Tracking");
    for(unsigned int index = 0;index < param_list.size();++index)
        if(s.contains(param_list[index]))
            set_data(param_list[index],s.value(param_list[index]));
}

void tracking_window::on_actionSave_Rendering_Parameters_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                           this,
                           "Save INI files",QFileInfo(windowTitle()).baseName()+".ini","Setting file (*.ini);;All files (*)");
    if (filename.isEmpty())
        return;
    QSettings s(filename, QSettings::IniFormat);
    QStringList param_list;
    param_list += renderWidget->getChildren("Rendering");
    param_list += renderWidget->getChildren("Slice");
    param_list += renderWidget->getChildren("Tract");
    param_list += renderWidget->getChildren("Region");
    param_list += renderWidget->getChildren("Surface");
    param_list += renderWidget->getChildren("ODF");
    for(unsigned int index = 0;index < param_list.size();++index)
        s.setValue(param_list[index],renderWidget->getData(param_list[index]));
}

void tracking_window::on_actionLoad_Rendering_Parameters_triggered()
{
    QString filename = QFileDialog::getOpenFileName(
                           this,"Open INI files",QFileInfo(windowTitle()).absolutePath(),"Setting file (*.ini);;All files (*)");
    if (filename.isEmpty())
        return;
    QSettings s(filename, QSettings::IniFormat);
    QStringList param_list;
    param_list += renderWidget->getChildren("Rendering");
    param_list += renderWidget->getChildren("Slice");
    param_list += renderWidget->getChildren("Tract");
    param_list += renderWidget->getChildren("Region");
    param_list += renderWidget->getChildren("Surface");
    param_list += renderWidget->getChildren("ODF");
    for(unsigned int index = 0;index < param_list.size();++index)
        if(s.contains(param_list[index]))
            set_data(param_list[index],s.value(param_list[index]));
    update_gl();
}

void tracking_window::on_addRegionFromAtlas_clicked()
{
    if(atlas_list.empty())
    {
        QMessageBox::information(0,"Error",QString("DSI Studio cannot find atlas files in ")+QCoreApplication::applicationDirPath()+ "/atlas",0);
        return;
    }
    if(!handle->can_map_to_mni())
    {
        QMessageBox::information(this,"Error","Atlas is not support for the current image resolution.",0);
        return;
    }
    std::auto_ptr<AtlasDialog> atlas_dialog(new AtlasDialog(this));
    if(atlas_dialog->exec() == QDialog::Accepted)
    {
        for(unsigned int i = 0;i < atlas_dialog->roi_list.size();++i)
            regionWidget->add_region_from_atlas(atlas_dialog->atlas_index,atlas_dialog->roi_list[i]);
        update_gl();
        scene.show_slice();
    }
}
void tracking_window::add_roi_from_atlas()
{
    if(!handle->can_map_to_mni())
        return;
    QStringList name_value = ui->search_atlas->text().split(":");
    if(name_value.size() != 2)
        return;
    for(int i = 0;i < atlas_list.size();++i)
        if(name_value[1].toStdString() == atlas_list[i].name)
        {
            for(int j = 0;j < atlas_list[i].get_list().size();++j)
            if(atlas_list[i].get_list()[j] == name_value[0].toStdString())
            {
                regionWidget->add_region_from_atlas(i,j);
                ui->search_atlas->setText("");
                update_gl();
                scene.show_slice();
                return;
            }
        }
}

void tracking_window::on_actionRestore_Settings_triggered()
{
    renderWidget->setDefault("ROI");
    renderWidget->setDefault("Rendering");
    renderWidget->setDefault("show_slice");
    renderWidget->setDefault("show_tract");
    renderWidget->setDefault("show_region");
    renderWidget->setDefault("show_surface");
    renderWidget->setDefault("show_odf");
    update_gl();
    scene.show_slice();
}


void tracking_window::on_actionRestore_Tracking_Settings_triggered()
{
    renderWidget->setDefault("Tracking");
    on_tracking_index_currentIndexChanged((*this)["tracking_index"].toInt());
    update_gl();
}

void tracking_window::on_zoom_in_clicked()
{
    set_data("roi_zoom",renderWidget->getData("roi_zoom").toInt()+1);
    scene.center();
    scene.show_slice();
}

void tracking_window::on_zoom_out_clicked()
{
    set_data("roi_zoom",std::max<int>(1,renderWidget->getData("roi_zoom").toInt()-1));
    scene.center();
    scene.show_slice();
}

std::pair<float,float> evaluate_fib(
        const image::geometry<3>& dim,
        const std::vector<std::vector<float> >& fib_fa,
        const std::vector<std::vector<float> >& fib_dir);
void tracking_window::on_actionQuality_Assessment_triggered()
{
    std::vector<std::vector<float> > fib_fa(handle->dir.num_fiber);
    std::vector<std::vector<float> > fib_dir(handle->dir.num_fiber);
    for(unsigned int i = 0;i < fib_fa.size();++i)
    {
        fib_fa[i].resize(handle->dim.size());
        std::copy(handle->dir.fa[i],handle->dir.fa[i]+handle->dim.size(),fib_fa[i].begin());
        fib_dir[i].resize(handle->dim.size()*3);
        for(unsigned int j = 0,index = 0;j < fib_dir[i].size();j += 3,++index)
        {
            const float* v = handle->dir.get_dir(index,i);
            fib_dir[i][j] = v[0];
            fib_dir[i][j+1] = v[1];
            fib_dir[i][j+2] = v[2];
        }
    }
    std::pair<float,float> result = evaluate_fib(handle->dim,fib_fa,fib_dir);
    std::ostringstream out;
    out << "Number of connected fibers: " << result.first << std::endl;
    out << "Number of disconnected fibers: " << result.second << std::endl;
    out << "Error ratio: " << 100.0*(float)result.second/(float)result.first << "%" << std::endl;
    show_info_dialog("Quality assessment",out.str().c_str());
}

#include "tessellated_icosahedron.hpp"
void tracking_window::on_actionImprove_Quality_triggered()
{
    tracking_data fib;
    fib.read(*handle);
    float threshold = 0.6*image::segmentation::otsu_threshold(image::make_image(handle->dir.fa[0],handle->dim));
    if(!fib.dir.empty())
        return;
    for(float cos_angle = 0.99;check_prog(1000-cos_angle*1000,1000-866);cos_angle -= 0.005)
    {
        std::vector<std::vector<float> > new_fa(handle->dir.num_fiber);
        std::vector<std::vector<short> > new_index(handle->dir.num_fiber);
        unsigned int size = handle->dim.size();
        for(unsigned int i = 0 ;i < new_fa.size();++i)
        {
            new_fa[i].resize(size);
            new_index[i].resize(size);
            std::copy(handle->dir.fa[i],handle->dir.fa[i]+size,new_fa[i].begin());
            std::copy(handle->dir.findex[i],handle->dir.findex[i]+size,new_index[i].begin());
        }

        for(image::pixel_index<3> index(handle->dim);index < handle->dim.size();++index)
        {
            if(handle->dir.fa[0][index.index()] < threshold)
                continue;
            std::vector<image::pixel_index<3> > neighbors;
            image::get_neighbors(index,handle->dim,neighbors);

            std::vector<image::vector<3> > dis(neighbors.size());
            std::vector<image::vector<3> > fib_dir(neighbors.size());
            std::vector<float> fib_fa(neighbors.size());


            for(unsigned char i = 0;i < neighbors.size();++i)
            {
                dis[i] = neighbors[i];
                dis[i] -= image::vector<3>(index);
                dis[i].normalize();
                unsigned char fib_order,reverse;
                if(fib.get_nearest_dir_fib(neighbors[i].index(),dis[i],fib_order,reverse,threshold,cos_angle))
                {
                    fib_dir[i] = handle->dir.get_dir(neighbors[i].index(),fib_order);
                    if(reverse)
                        fib_dir[i] = -fib_dir[i];
                    fib_fa[i] = handle->dir.fa[fib_order][neighbors[i].index()];
                }
            }


            for(unsigned char i = 0;i < neighbors.size();++i)
            if(fib_fa[i] > threshold)
            {
                for(unsigned char j = i+1;j < neighbors.size();++j)
                if(fib_fa[j] > threshold)
                {
                    float angle = fib_dir[i]*fib_dir[j];
                    if(angle > -cos_angle) // select opposite side
                        continue;
                    image::vector<3> predict_dir(fib_dir[i]);
                    if(angle > 0)
                        predict_dir += fib_dir[j];
                    else
                        predict_dir -= fib_dir[j];
                    predict_dir.normalize();
                    unsigned char fib_order,reverse;
                    bool has_match = false;
                    if(fib.get_nearest_dir_fib(index.index(),predict_dir,fib_order,reverse,threshold,cos_angle))
                    {
                        if(reverse)
                            predict_dir -= image::vector<3>(handle->dir.get_dir(index.index(),fib_order));
                        else
                            predict_dir += image::vector<3>(handle->dir.get_dir(index.index(),fib_order));
                        predict_dir.normalize();
                        has_match = true;
                    }
                    short dir_index = 0;
                    float max_value = 0.0;
                    for (unsigned int k = 0; k < handle->dir.half_odf_size; ++k)
                    {
                        float value = std::abs(predict_dir*handle->dir.odf_table[k]);
                        if (value > max_value)
                        {
                            max_value = value;
                            dir_index = k;
                        }
                    }

                    if(has_match)
                        new_index[fib_order][index.index()] = dir_index;
                    else
                    {
                        float add_fa = (fib_fa[i]+fib_fa[j])*0.5;
                        for(unsigned char m = 0;m < new_fa.size();++m)
                        if(add_fa > new_fa[m][index.index()])
                        {
                            std::swap(add_fa,new_fa[m][index.index()]);
                            std::swap(dir_index,new_index[m][index.index()]);
                        }
                    }
                }
            }
        }
        for(unsigned int i = 0 ;i < new_fa.size();++i)
        {
            std::copy(new_fa[i].begin(),new_fa[i].begin()+size,(float*)handle->dir.fa[i]);
            std::copy(new_index[i].begin(),new_index[i].begin()+size,(short*)handle->dir.findex[i]);
        }
    }
    scene.show_slice();
}

void tracking_window::on_actionAuto_Rotate_triggered(bool checked)
{
    if(checked)
        {
            glWidget->time.start();
            glWidget->last_time = glWidget->time.elapsed();
            timer.reset(new QTimer());
            timer->setInterval(1);
            connect(timer.get(), SIGNAL(timeout()), glWidget, SLOT(rotate()));
            timer->start();
        }
    else
        timer->stop();
    if(ui->auto_rotate->isChecked() != checked)
        ui->auto_rotate->setChecked(checked);
}

void tracking_window::on_auto_rotate_toggled(bool checked)
{
    if(ui->actionAuto_Rotate->isChecked() != checked)
        ui->actionAuto_Rotate->setChecked(checked);
    on_actionAuto_Rotate_triggered(checked);
}

void tracking_window::on_action3D_Screen_triggered()
{
    glWidget->copyToClipboard();
}

void tracking_window::on_action3D_Screen_3_Views_triggered()
{
    QImage all;
    glWidget->get3View(all,0);
    QApplication::clipboard()->setImage(all);
}

void tracking_window::on_action3D_Screen_3_Views_Horizontal_triggered()
{
    QImage all;
    glWidget->get3View(all,1);
    QApplication::clipboard()->setImage(all);
}

void tracking_window::on_action3D_Screen_3_Views_Vertical_triggered()
{
    QImage all;
    glWidget->get3View(all,2);
    QApplication::clipboard()->setImage(all);
}

void tracking_window::on_actionROI_triggered()
{
    scene.copyClipBoard();
}

void tracking_window::on_actionTrack_Report_triggered()
{
    if(tact_report_imp.get())
                tact_report_imp->copyToClipboard();
}
QString tracking_window::get_save_file_name(QString title,QString file_name,QString file_type)
{
    return QFileDialog::getSaveFileName(this,title,file_name,file_type);
}

void tracking_window::show_info_dialog(const std::string& title,const std::string& result)
{
    QMessageBox msgBox;
    msgBox.setText(title.c_str());
    msgBox.setDetailedText(result.c_str());
    msgBox.setStandardButtons(QMessageBox::Ok|QMessageBox::Save);
    msgBox.setDefaultButton(QMessageBox::Ok);
    QPushButton *copyButton = msgBox.addButton("Copy To Clipboard", QMessageBox::ActionRole);
    if(msgBox.exec() == QMessageBox::Save)
    {
        QString filename;
        filename = QFileDialog::getSaveFileName(this,
                    "Save as",QFileInfo(windowTitle()).baseName()+"_info.txt",
                    "Text files (*.txt);;All files|(*)");
        if(filename.isEmpty())
            return;
        std::ofstream out(filename.toLocal8Bit().begin());
        out << result.c_str();
    }
    if (msgBox.clickedButton() == copyButton)
        QApplication::clipboard()->setText(result.c_str());
}





void tracking_window::on_rendering_efficiency_currentIndexChanged(int index)
{
    if(!renderWidget)
        return;
    switch(index)
    {
    case 0:
        set_data("line_smooth",0);
        set_data("point_smooth",0);

        set_data("tract_style",0);
        set_data("tract_visible_tract",10000);
        set_data("tract_tube_detail",0);

        break;
    case 1:
        set_data("line_smooth",0);
        set_data("point_smooth",0);

        set_data("tract_style",1);
        set_data("tract_visible_tract",25000);
        set_data("tract_tube_detail",1);
        break;
    case 2:
        set_data("line_smooth",1);
        set_data("point_smooth",1);

        set_data("tract_style",1);
        set_data("tract_visible_tract",100000);
        set_data("tract_tube_detail",3);
        break;
    }
    update_gl();
}

void tracking_window::on_load_color_map_clicked()
{
    QMessageBox::information(this,"Load color map","Please assign a text file of RGB numbers as the colormap.");
    QString filename;
    filename = QFileDialog::getOpenFileName(this,
                "Load color map","color_map.txt",
                "Text files (*.txt);;All files|(*)");
    if(filename.isEmpty())
        return;
    image::color_map_rgb new_color_map;
    if(!new_color_map.load_from_file(filename.toStdString().c_str()))
    {
          QMessageBox::information(this,"Error","Invalid color map format");
          return;
    }
    v2c.set_color_map(new_color_map);
    scene.show_slice();
}
void tracking_window::on_load_color_map_gl_released()
{
    QMessageBox::information(this,"Load color map","Please assign a text file of RGB numbers as the colormap.");
    QString filename;
    filename = QFileDialog::getOpenFileName(this,
                "Load color map","color_map.txt",
                "Text files (*.txt);;All files|(*)");
    if(filename.isEmpty())
        return;
    image::color_map_rgb new_color_map;
    if(!new_color_map.load_from_file(filename.toStdString().c_str()))
    {
          QMessageBox::information(this,"Error","Invalid color map format");
          return;
    }
    v2c_gl.set_color_map(new_color_map);
    glWidget->update_slice();
}


void tracking_window::on_max_value_valueChanged(double arg1)
{
    v2c.set_range(ui->min_value->value(),ui->max_value->value());
    scene.show_slice();
}

void tracking_window::on_min_value_valueChanged(double arg1)
{
    v2c.set_range(ui->min_value->value(),ui->max_value->value());
    scene.show_slice();
}

void tracking_window::on_max_color_released()
{
    v2c.two_color(ui->min_color->color().rgb(),ui->max_color->color().rgb());
    scene.show_slice();
}

void tracking_window::on_min_color_released()
{
    v2c.two_color(ui->min_color->color().rgb(),ui->max_color->color().rgb());
    scene.show_slice();
}


void tracking_window::on_actionCut_X_triggered()
{
    tractWidget->cut_by_slice(0,true);
}

void tracking_window::on_actionCut_X_2_triggered()
{
    tractWidget->cut_by_slice(0,false);
}

void tracking_window::on_actionCut_Y_triggered()
{
    tractWidget->cut_by_slice(1,true);
}

void tracking_window::on_actionCut_Y_2_triggered()
{
    tractWidget->cut_by_slice(1,false);
}

void tracking_window::on_actionCut_Z_triggered()
{
    tractWidget->cut_by_slice(2,true);
}

void tracking_window::on_actionCut_Z_2_triggered()
{
    tractWidget->cut_by_slice(2,false);
}

void tracking_window::on_actionStrip_skull_for_T1w_image_triggered()
{
    if(glWidget->current_visible_slide)
    {
        image::basic_image<float,3> tmp = other_slices[glWidget->current_visible_slide-1]->source_images;
        other_slices[glWidget->current_visible_slide-1]->stripskull(renderWidget->getData("fa_threshold").toFloat());
        glWidget->addSurface();
        other_slices[glWidget->current_visible_slide-1]->source_images = tmp;
    }
    else
        QMessageBox::information(this,"Error","Load T1W image first");
}

void tracking_window::on_show_fiber_toggled(bool checked)
{
    ui->show_fiber->setChecked(checked);
    if(ui->show_fiber->isChecked() ^ (*this)["roi_fiber"].toBool())
        set_data("roi_fiber",ui->show_fiber->isChecked());
    scene.show_slice();
}

void tracking_window::on_show_r_toggled(bool checked)
{
    ui->show_r->setChecked(checked);
    if(ui->show_r->isChecked() ^ (*this)["roi_label"].toBool())
        set_data("roi_label",ui->show_r->isChecked());
    scene.show_slice();
}

void tracking_window::on_show_position_toggled(bool checked)
{
    ui->show_position->setChecked(checked);
    if(ui->show_position->isChecked() ^ (*this)["roi_position"].toBool())
        set_data("roi_position",ui->show_position->isChecked());
    scene.show_slice();
}

void tracking_window::on_actionIndividual_Connectometry_triggered()
{
    if(!handle->is_qsdr)
    {
        QMessageBox::information(this,"Error","Please open an atlas in STEP3: fiber tracking to run individual connectometry. See online documentation for details.");
        return;
    }
    std::shared_ptr<individual_connectometry> indi(new individual_connectometry(this,*this));
    indi->exec();
}

void tracking_window::on_actionAdjust_Mapping_triggered()
{
    int current_visible_slide = ui->SliceModality->currentIndex();
    if(!current_visible_slide)
        return;
    std::auto_ptr<manual_alignment> manual(new manual_alignment(this,
        slice.source_images,slice.voxel_size,
        other_slices[current_visible_slide-1]->source_images,other_slices[current_visible_slide-1]->voxel_size,
            image::reg::rigid_body,image::reg::reg_cost_type::mutual_info));
    handle->reg.set_arg(other_slices[current_visible_slide-1]->arg_min);
    manual->timer->start();
    if(manual->exec() != QDialog::Accepted)
        return;
    other_slices[current_visible_slide-1]->terminate();
    other_slices[current_visible_slide-1]->arg_min = manual->data.get_arg();
    other_slices[current_visible_slide-1]->update();
    update_gl();
}

void tracking_window::on_actionSave_mapping_triggered()
{
    int current_visible_slide = ui->SliceModality->currentIndex();
    if(!current_visible_slide)
        return;
    QString filename = QFileDialog::getSaveFileName(
            this,
            "Save Mapping Matrix",QFileInfo(windowTitle()).completeBaseName()+".mapping.txt",
            "Text files (*.txt);;All files (*)");
    if(filename.isEmpty())
        return;
    std::ofstream out(filename.toLocal8Bit().begin());

    for(int row = 0,index = 0;row < 4;++row)
    {
        for(int col = 0;col < 4;++col,++index)
            out << other_slices[current_visible_slide-1]->transform[index] << " ";
        out << std::endl;
    }
}

void tracking_window::on_actionLoad_mapping_triggered()
{
    int current_visible_slide = ui->SliceModality->currentIndex();
    if(!current_visible_slide)
        return;
    QString filename = QFileDialog::getOpenFileName(
            this,"Open Mapping Matrix",QFileInfo(windowTitle()).absolutePath(),"Text files (*.txt);;All files (*)");
    std::ifstream in(filename.toLocal8Bit().begin());
    if(filename.isEmpty() || !in)
        return;
    other_slices[current_visible_slide-1]->terminate();
    std::vector<float> data;
    std::copy(std::istream_iterator<float>(in),
              std::istream_iterator<float>(),std::back_inserter(data));
    data.resize(16);
    data[15] = 1.0;
    other_slices[current_visible_slide-1]->transform = data;
    other_slices[current_visible_slide-1]->invT = data;
    other_slices[current_visible_slide-1]->invT.inv();
    other_slices[current_visible_slide-1]->update_roi();
    update_gl();
}

bool tracking_window::addSlices(QStringList filenames,bool correct_intensity,bool cmd)
{
    std::vector<std::string> files(filenames.size());
    for (unsigned int index = 0; index < filenames.size(); ++index)
            files[index] = filenames[index].toLocal8Bit().begin();
    std::shared_ptr<CustomSliceModel> new_slice(new CustomSliceModel);
    if(!new_slice->initialize(slice,handle->is_qsdr,files,correct_intensity))
    {
        if(!cmd)
            QMessageBox::information(this,"Error reading image files",0);
        return false;
    }
    for(int i = 0;i < other_slices.size();++i)
        if(new_slice->name == other_slices[i]->name)
        {
            new_slice->name += "'";
            i = -1;
        }
    other_slices.push_back(new_slice);
    glWidget->current_visible_slide = other_slices.size();
    if(!cmd && !timer2.get())
    {
        timer2.reset(new QTimer());
        timer2->setInterval(200);
        connect(timer2.get(), SIGNAL(timeout()), this, SLOT(check_reg()));
        timer2->start();
    }
    return true;
}

void tracking_window::check_reg(void)
{
    bool all_ended = true;
    for(unsigned int index = 0;index < other_slices.size();++index)
    {
        if(!other_slices[index]->ended)
        {
            all_ended = false;
            other_slices[index]->update();
        }
    }
    scene.show_slice();
    if(all_ended)
        timer2.reset(0);
    else
        update_gl();
}

void tracking_window::update_gl(void)
{
    glWidget->updateGL();
    if(glWidget2)
        glWidget2->updateGL();
}

void tracking_window::on_glView_currentIndexChanged(int index)
{
    if(index && !glWidget2)
    {
        ui->glLayout->addWidget(glWidget2 = new GLWidget(renderWidget->getData("anti_aliasing").toInt(),*this,renderWidget));
        glWidget2->current_visible_slide = glWidget->current_visible_slide;
        connect(tractWidget,SIGNAL(need_update()),glWidget2,SLOT(makeTracts()));
        connect(tractWidget,SIGNAL(need_update()),glWidget2,SLOT(updateGL()));
        glWidget2->bind_rotate = renderWidget->getData("stereoscopy_angle").toFloat();
    }
    if(!index && glWidget2)
    {
        glWidget->bind_gl = 0;
        delete glWidget2;
        glWidget2 = 0;
    }
    glWidget->bind_gl = (index == 2) ? glWidget2: 0;
    if(glWidget2)
        glWidget2->bind_gl = (index == 2) ? glWidget: 0;
    update_gl();
}
