#include <chrono>
#include <thread>
#include <mutex>
#include <sstream>
#include <condition_variable>
#include <ros/ros.h>
#include <boost/thread.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <std_msgs/String.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Int32.h>
#include <std_srvs/Trigger.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <cv_bridge/cv_bridge.h>
#include "CameraYCAM3D.hpp"
#include "ElapsedTimer.hpp"
#include "rovi/Floats.h"
#include "rovi/GenPC.h"
#include "rovi/ImageFilter.h"


namespace {
//============================================= �������O��� start =============================================
//#define DEBUG_DETAIL
#define LOG_HEADER "(ycam3d) "

const std::string FRAME_ID = "camera";
	
enum YCam3DMode {
	Mode_StandBy = 1,
	Mode_Streaming = 2
};
	
ros::NodeHandle *nh = nullptr;

ros::Publisher pub_img_raws[2];
ros::Publisher pub_Y1;
ros::Publisher pub_pcount;
ros::Publisher pub_stat;
ros::Publisher pub_error;
ros::Publisher pub_info;

ros::Publisher pub_rects[2];
ros::Publisher pub_rects0[2];
ros::Publisher pub_rects1[2];
ros::Publisher pub_diffs[2];
ros::Publisher pub_views[2];

ros::ServiceClient svc_genpc;
ros::ServiceClient svc_remap[2];

std::string cam_ipaddr;
int cam_width = -1;
int cam_height = -1;

const std::string PRM_MODE                    = "ycam/Mode";
const std::string PRM_CAM_OPEN_STAT           = "ycam/stat";
const std::string PRM_SW_TRIG_RATE            = "ycam/SoftwareTriggerRate";
const std::string PRM_EXPOSURE_TIME_LEVEL     = "ycam/ExposureTimeLevel";
const std::string PRM_DRAW_CAMERA_ORIGIN      = "ycam/DrawCameraOrigin";
//const std::string PRM_CAM_EXPSR_TM          = "ycam/camera/ExposureTime";
const std::string PRM_CAM_GAIN_D              = "ycam/camera/Gain";
//const std::string PRM_CAM_GAIN_A            = "ycam/camera/GainA";
//const std::string PRM_PROJ_EXPSR_TM         = "ycam/projector/ExposureTime";
const std::string PRM_PROJ_INTENSITY          = "ycam/projector/Intensity";

const std::string PRM_NW_DELAY_MON_ENABLED       = "ycam/nw_delay_monitor/enabled";
const std::string PRM_NW_DELAY_MON_INTERVAL      = "ycam/nw_delay_monitor/interval";
const std::string PRM_NW_DELAY_MON_TIMEOUT       = "ycam/nw_delay_monitor/timeout";
const std::string PRM_NW_DELAY_MON_IGN_UPD_FAIL  = "ycam/nw_delay_monitor/ignore_update_failure";
const std::string PRM_CAM_CALIB_MAT_K_LIST[]  = {"left/remap/Kn","right/remap/Kn"};
	
constexpr int PRM_SW_TRIG_RATE_DEFAULT = 2; //Hz
constexpr int PRM_SW_TRIG_RATE_MAX = 6; //Hz
constexpr int PRM_SW_TRIG_RATE_MIN = 1; //Hz

constexpr int YCAM_STAND_BY_MODE_CYCLE = 3; //Hz
	
int pre_ycam_mode = (int)Mode_StandBy;

std::string camera_res;
std::unique_ptr<CameraYCAM3D> camera_ptr;

ros::Timer mode_mon_timer;
int cur_mode_mon_cyc = YCAM_STAND_BY_MODE_CYCLE; //Hz

ros::Timer cam_open_mon_timer;

bool cam_params_refreshed=false;

int pre_cam_gain_d    = 0;
int pre_proj_intensity = 0;

std::timed_mutex pc_gen_mutex;
std::mutex ptn_capt_wait_mutex;
std::condition_variable ptn_capt_wait_cv;
std::thread pc_gen_thread;

int g_node_exit_flg = 0;

std::vector<camera::ycam3d::CameraImage> ptn_imgs_l;
std::vector<camera::ycam3d::CameraImage> ptn_imgs_r;

int expsr_tm_lv_ui_default = -1;
int expsr_tm_lv_ui_min = -1;
int expsr_tm_lv_ui_max = -1;
	
//**********DEBUG
//int debug_pre_strobe = 0;
	
template<typename T>
T get_param(const std::string &key,const T default_val){
	T val = default_val;
	if( ! nh->getParam(key,val) ) {
		ROS_ERROR(LOG_HEADER"error:parameter get failed. key=%s",key.c_str());
	}
	return val;
}

template<typename T>
T get_param(const std::string &key,const T default_val,const T min_val,const T max_val){
	T val = default_val;
	if( ! nh->getParam(key,val) ) {
		ROS_ERROR(LOG_HEADER"error:parameter get failed. key=%s",key.c_str());
		
	}else if( val < min_val){
		ROS_ERROR(LOG_HEADER"error:current parameter value is below minimum value. key=%s val=%d min_val=%d",key.c_str(),val,min_val);
		val = min_val;
		nh->setParam(key,min_val);
		
	}else if( max_val < val){
		val = max_val;
		nh->setParam(key,max_val);
		ROS_ERROR(LOG_HEADER"error:current parameter value is above maximum value. key=%s",key.c_str());
		
	}
	return val;
}
	
void publish_bool(ros::Publisher&pub, const bool val){
	std_msgs::Bool rmsg;
	rmsg.data = val;
	pub.publish(rmsg);
}

void publish_int32(ros::Publisher&pub, const int &val){
	std_msgs::Int32 rmsg;
	rmsg.data = val;
	pub.publish(rmsg);
}

void publish_string(ros::Publisher&pub, const std::string &val){
	std_msgs::String rmsg;
	rmsg.data = val;
	pub.publish(rmsg);
}
	
bool init(){
	bool ret=false;
	if( ! nh->getParam("camera/address",cam_ipaddr) ){
		ROS_ERROR(LOG_HEADER"error:camera ipaddr get failed.");
		
	}else if( ! nh->getParam("camera/Width", cam_width) ){
		ROS_ERROR(LOG_HEADER"error:camera width get failed.");
		
	}else if( ! nh->getParam("camera/Height", cam_height) ){
		ROS_ERROR(LOG_HEADER"error:camera height get failed.");
		
	}else if( ! nh->getParam("camera/ID",camera_res) ){
		ROS_ERROR(LOG_HEADER"error:camera resolution value not found.");
		
	}else{
		pre_ycam_mode = get_param<int>(PRM_MODE,(int)Mode_StandBy);
		if(pre_ycam_mode == Mode_Streaming){
			cur_mode_mon_cyc = get_param<int>(PRM_SW_TRIG_RATE,PRM_SW_TRIG_RATE_DEFAULT);
		}else{
			cur_mode_mon_cyc = YCAM_STAND_BY_MODE_CYCLE;
		}
				
		ret=true;
	}
	return ret;
}

void update_camera_params(){
	if( ! cam_params_refreshed ){
		ROS_WARN(LOG_HEADER"camera parameters have not been refreshed.");
		return;
	}
	
	ElapsedTimer tmr;
	if( ! camera_ptr || ! camera_ptr->is_open()){
		ROS_ERROR(LOG_HEADER"error:camera is not opened.");
		return;
	}
	if( expsr_tm_lv_ui_default < 0 || expsr_tm_lv_ui_min < 0 || expsr_tm_lv_ui_max < 0 ){
		//skip
	}else{
		int cur_expsr_lv_raw = -1;
		if( ! camera_ptr->get_exposure_time_level(&cur_expsr_lv_raw) ){
			ROS_ERROR(LOG_HEADER"error:current exposure time level get failed.");
		}else{
			
			const int cur_expsr_lv_ui = get_param<int>(PRM_EXPOSURE_TIME_LEVEL, expsr_tm_lv_ui_default);
			if( cur_expsr_lv_ui < expsr_tm_lv_ui_min ){
				ROS_ERROR(LOG_HEADER"error:'exposure time level' is below minimum value. val=%d min=%d",cur_expsr_lv_ui,expsr_tm_lv_ui_min);
				nh->setParam(PRM_EXPOSURE_TIME_LEVEL, cur_expsr_lv_raw + 1 );
				
			}else if( expsr_tm_lv_ui_max < cur_expsr_lv_ui ){
				ROS_ERROR(LOG_HEADER"error:'exposure time level' is above maximum value. val=%d max=%d",cur_expsr_lv_ui,expsr_tm_lv_ui_max);
				nh->setParam(PRM_EXPOSURE_TIME_LEVEL, cur_expsr_lv_raw + 1 );
				
			}else{
				//ui�p��1�v���X����Ă���ƍl����
				if( cur_expsr_lv_raw != cur_expsr_lv_ui - 1 ){
					//ROS_WARN(LOG_HEADER"exposure time level change start.");
					if( ! camera_ptr->set_exposure_time_level(cur_expsr_lv_ui - 1) ){
						ROS_ERROR(LOG_HEADER"error:'exposure time level' set failed. val=%d",cur_expsr_lv_ui);
						nh->setParam(PRM_EXPOSURE_TIME_LEVEL, cur_expsr_lv_raw + 1 );
					}else{
						ROS_INFO(LOG_HEADER"'exposure time level' paramter changed. old=%d new=%d", cur_expsr_lv_raw + 1 , cur_expsr_lv_ui);
					}
					//ROS_WARN(LOG_HEADER"exposure time level change end.");
				}
			}
			
		}
	}
	
	
	const int cur_cam_gain_d = get_param<int>( PRM_CAM_GAIN_D,
		camera::ycam3d::CAM_DIGITAL_GAIN_DEFAULT, camera::ycam3d::CAM_DIGITAL_GAIN_MIN, camera::ycam3d::CAM_DIGITAL_GAIN_MAX);
	if( pre_cam_gain_d != cur_cam_gain_d ){
		if( ! camera_ptr->set_gain_digital(cur_cam_gain_d) ){
			int latest_val=0;
			if( ! camera_ptr->get_gain_digital(&latest_val) ){
				ROS_ERROR(LOG_HEADER"error:'camera digital gain' get failed.");
			}else{
				ROS_ERROR(LOG_HEADER"error:'camera digital gain' set failed. set_val=%d cur_val=%d", cur_cam_gain_d, latest_val);
				nh->setParam(PRM_CAM_GAIN_D, latest_val);
				pre_cam_gain_d = latest_val;
			}
		}else{
			ROS_INFO(LOG_HEADER"'camera digital gain' paramter changed. old=%d new=%d", pre_cam_gain_d, cur_cam_gain_d);
			pre_cam_gain_d = cur_cam_gain_d;
		}
	}
	
	const int cur_proj_intensty = get_param<int>( PRM_PROJ_INTENSITY,
		camera::ycam3d::PROJ_FLASH_INTERVAL_DEFAULT, camera::ycam3d::PROJ_FLASH_INTERVAL_MIN, camera::ycam3d::PROJ_FLASH_INTERVAL_MAX );	
	if( pre_proj_intensity != cur_proj_intensty ){
		if( ! camera_ptr->set_projector_brightness(cur_proj_intensty) ){
			int latest_val = 0;
			if( ! camera_ptr->get_projector_brightness(&latest_val) ){
				ROS_ERROR(LOG_HEADER"error:'projector intensity' get failed.");
			}else{
				ROS_ERROR(LOG_HEADER"error:'projector intensity' set failed. set_val=%d cur_val=%d", cur_proj_intensty, latest_val);
				nh->setParam(PRM_PROJ_INTENSITY, latest_val);
				pre_proj_intensity = latest_val;
			}
		}else{
			ROS_INFO(LOG_HEADER"'projector intensity' paramter changed. old=%d new=%d", pre_proj_intensity, cur_proj_intensty);
			pre_proj_intensity = cur_proj_intensty;
		}
	}
	
	//ROS_INFO(LOG_HEADER"camera parameter updated. elapsed=%d ms",tmr.elapsed_ms());
}

void cam_open_monitor_task(const ros::TimerEvent& e){
	if( ! camera_ptr || ! camera_ptr->is_open()){
		return;
	}
	publish_bool(pub_stat,true);
}

void mode_monitor_task(const ros::TimerEvent& e){
	const int cur_ycam_mode = get_param<int>(PRM_MODE,(int)Mode_StandBy);
	
	if( ! camera_ptr ){
		return;
	}
	
	if( ! pc_gen_mutex.try_lock_for(std::chrono::seconds(0)) ){
		ROS_WARN(LOG_HEADER"genpc is working. live was skipped.");
		return;
	}
	pc_gen_mutex.unlock();
	
	
	//ROS_INFO(LOG_HEADER"ycam mode=%d ",cur_ycam_mode);
	if( ! camera_ptr->is_open() ){
		//ROS_ERROR(LOG_HEADER"error:camera disconnect.");
		//ROS_WARN(LOG_HEADER"error:camera is disconnected. trying to connect ...");
		nh->setParam(PRM_CAM_OPEN_STAT,false);
		return;
	}else{
		nh->setParam(PRM_CAM_OPEN_STAT,true);
	}
	
	//�p�����[�^�擾
	update_camera_params();
	
	if( cur_ycam_mode == Mode_Streaming ){
		if(camera_ptr->is_busy()){
			ROS_WARN(LOG_HEADER"camera is busy.!!!!");
		}else{
			//ROS_WARN(LOG_HEADER"live capture start.");
			int strobe=0;
			if(nh->getParam("ycam/Strobe",strobe) && strobe != 0){
				camera_ptr->capture_strobe();
			}else{
				camera_ptr->capture();
			}
			//debug_pre_strobe = strobe;
		}
	}else{
		//todo ******************* pending *******************
	}
	
	//�Ď������ύX
	if( pre_ycam_mode != cur_ycam_mode){
		ROS_INFO(LOG_HEADER"switched. pre=%d => cur=%d",pre_ycam_mode,cur_ycam_mode);
		
		std::string mode_name;
		if( cur_ycam_mode == Mode_Streaming ){
			cur_mode_mon_cyc = get_param<int>(PRM_SW_TRIG_RATE, PRM_SW_TRIG_RATE_DEFAULT, PRM_SW_TRIG_RATE_MIN, PRM_SW_TRIG_RATE_MAX);
			mode_name = "Streaming";
			
		}else{
			cur_mode_mon_cyc = YCAM_STAND_BY_MODE_CYCLE;
			mode_name = "StandBy";
			
		}
		
		ROS_INFO(LOG_HEADER"changed to '%s' mode. cycle=%d Hz",mode_name.c_str(),cur_mode_mon_cyc);
		mode_mon_timer.setPeriod(ros::Duration(1/(float)cur_mode_mon_cyc),true);
		
	}else if( cur_ycam_mode == Mode_Streaming ){
		const int mode_mon_cyc = get_param<int>(PRM_SW_TRIG_RATE, PRM_SW_TRIG_RATE_DEFAULT, PRM_SW_TRIG_RATE_MIN, PRM_SW_TRIG_RATE_MAX);
		if( cur_mode_mon_cyc != mode_mon_cyc ){
			ROS_INFO(LOG_HEADER"'Streaming' mode cycle changed. %d Hz => %d Hz",cur_mode_mon_cyc,mode_mon_cyc);
			
			mode_mon_timer.setPeriod(ros::Duration(1/(float)mode_mon_cyc),true);
			cur_mode_mon_cyc = mode_mon_cyc;
		}
	}
	
	pre_ycam_mode = cur_ycam_mode;
}

void on_camera_open_finished(const bool result){
	ROS_INFO(LOG_HEADER"camera opened. result=%s",(result?"OK":"NG"));
	
	expsr_tm_lv_ui_default = -1;
	expsr_tm_lv_ui_min = -1;
	expsr_tm_lv_ui_max = -1;
	
	if( ! result ){
		return;
	}

	int expsr_tm_lv_raw_default = -1;
	int expsr_tm_lv_raw_min = -1;
	int expsr_tm_lv_raw_max = -1;
	if( ! camera_ptr->get_exposure_time_level_default(&expsr_tm_lv_raw_default) ){
		ROS_ERROR(LOG_HEADER"exposure time level default val get failed.");
	}else if( ! camera_ptr->get_exposure_time_level_min(&expsr_tm_lv_raw_min) ){
		ROS_ERROR(LOG_HEADER"exposure time level min val get failed.");
	}else if( ! camera_ptr->get_exposure_time_level_max(&expsr_tm_lv_raw_max) ){
		ROS_ERROR(LOG_HEADER"exposure time level max val get failed.");
	}else{
		expsr_tm_lv_ui_default = expsr_tm_lv_raw_default + 1;
		expsr_tm_lv_ui_min = expsr_tm_lv_raw_min + 1;
		expsr_tm_lv_ui_max = expsr_tm_lv_raw_max + 1;
		
		ROS_INFO(LOG_HEADER"exposure time level: min=%d max=%d default=%d",
			expsr_tm_lv_ui_min, expsr_tm_lv_ui_max, expsr_tm_lv_ui_default);
	}
	
	pre_cam_gain_d = get_param<int>( PRM_CAM_GAIN_D,
		camera::ycam3d::CAM_DIGITAL_GAIN_DEFAULT, camera::ycam3d::CAM_DIGITAL_GAIN_MIN, camera::ycam3d::CAM_DIGITAL_GAIN_MAX);
	
	pre_proj_intensity = get_param<int>( PRM_PROJ_INTENSITY,
		camera::ycam3d::PROJ_FLASH_INTERVAL_DEFAULT, camera::ycam3d::PROJ_FLASH_INTERVAL_MIN, camera::ycam3d::PROJ_FLASH_INTERVAL_MAX );	
	
	ROS_INFO(LOG_HEADER"camera digital gain = %d", pre_cam_gain_d);
	ROS_INFO(LOG_HEADER"projector intensity = %d", pre_proj_intensity);
	
	cam_params_refreshed=true;
	if( ! result ){
		publish_string(pub_error,"YCAM open failed.");
	}else{
		publish_string(pub_info,"YCAM ready.");
	}
}

void on_camera_disconnect(){
	ROS_ERROR(LOG_HEADER"error: camera disconnected.");
	cam_params_refreshed=false;
	
	publish_bool(pub_stat,false);
	publish_string(pub_error,"YCAM disconected");
}
	
void on_camera_closed(){
	ROS_INFO(LOG_HEADER"camera closed.");
	cam_params_refreshed=false;
	
	publish_bool(pub_stat,false);
	publish_string(pub_info,"YCAM closed");
}

	
sensor_msgs::Image to_diff_img(const sensor_msgs::Image &src_img,sensor_msgs::Image &dst_img){
	sensor_msgs::Image diff_img;
	if( src_img.data.empty() ||
		src_img.data.size() != dst_img.data.size()
	){
		ROS_ERROR(LOG_HEADER"error:camera image size is different");
	}else{
		diff_img = src_img;
		for ( int n=0; n < src_img.data.size(); n++ ) {
			int val = dst_img.data[n] - src_img.data[n];
			if (val < 1) { val = 0; }
			else if (val>255) { val=255; }
			
			diff_img.data[n] = val;
		}
	}
	
	return diff_img;
}


sensor_msgs::Image drawCameraOriginCross(sensor_msgs::Image &inputImg,cv::Point &posCross){
	const int width = inputImg.width;
	const int height = inputImg.height;
	if( inputImg.encoding.compare(sensor_msgs::image_encodings::MONO8) != 0){
		ROS_ERROR(LOG_HEADER"error:center cross draw failed. unsupport image encoding.");
		return inputImg;
	}
	
	cv::Mat grayImg = cv_bridge::toCvCopy(inputImg, sensor_msgs::image_encodings::MONO8)->image;
	
	cv_bridge::CvImage colorImg;
	cv::cvtColor(grayImg,colorImg.image, cv::COLOR_GRAY2RGB);
	
	const int size_gcd = std::min(width,height);
	const int cross_width = std::max(size_gcd / 150,1);
	const int cross_len = std::max(size_gcd/30,3);
	
	const int cx = posCross.x;//width /2;
	const int cy = posCross.y;//height/2;
	
	cv::line(colorImg.image, cv::Point(cx - cross_len/2  , cy) , cv::Point(cx + cross_len/2, cy), cv::Scalar(255,0,0),cross_width , CV_AA);
	cv::line(colorImg.image, cv::Point(cx, cy - cross_len/2),    cv::Point(cx, cy + cross_len/2)   , cv::Scalar(255,0,0),cross_width , CV_AA);
	
	cv::imwrite("/tmp/color.png",colorImg.image);
	
	sensor_msgs::Image outputImg;
	outputImg=*colorImg.toImageMsg();
	outputImg.header = inputImg.header;
	outputImg.encoding = sensor_msgs::image_encodings::RGB8;
	
	return outputImg;
}

	
void on_capture_image_received(const bool result,const int proc_tm,const std::vector<camera::ycam3d::CameraImage> &imgs_l,const std::vector<camera::ycam3d::CameraImage> &imgs_r,const bool timeout){
	//ROS_INFO(LOG_HEADER"capture image recevie start. result=%s, proc_tm=%d ms, timeout=%d imgs_l: result=%d size=%d x %d, imgs_r: result=%d size=%d x %d",
	//	(result?"OK":"NG"), proc_tm, timeout, img_l.result, img_l.width, img_l.height, img_r.result, img_r.width, img_r.height);
	
	if( ! result ){
		ROS_ERROR(LOG_HEADER"error:capture failed.");
		return;
	}
	
	const ros::Time now = ros::Time::now();
	
	ElapsedTimer tmr;
	sensor_msgs::Image ros_imgs_darks[2];
	sensor_msgs::Image ros_imgs_brights[2];
		
	const camera::ycam3d::CameraImage cam_imgs_darks[2] = { imgs_l.at(0), imgs_r.at(0) };
	
	const camera::ycam3d::CameraImage cam_imgs_brights[2] = { imgs_l.at(1), imgs_r.at(1) };
	const bool drawCameraOriginCrossFlg =  get_param<bool>(PRM_DRAW_CAMERA_ORIGIN,false);
	for (int i = 0 ; i < 2 ; ++i ){
		cam_imgs_brights[i].to_ros_img(ros_imgs_brights[i],FRAME_ID);
		pub_img_raws[i].publish(ros_imgs_brights[i]);
		
		cam_imgs_darks[i].to_ros_img(ros_imgs_darks[i],FRAME_ID);
		
		
		//remap
		sensor_msgs::Image remap_img_bright;
		sensor_msgs::Image remap_img_dark;
		{
			//remap-bright
			rovi::ImageFilter remap_img_filter;
			remap_img_filter.request.img = ros_imgs_brights[i];
			//ROS_INFO(LOG_HEADER"remap start. camno=%d",i);
			if( ! svc_remap[i].call(remap_img_filter) ){
				ROS_ERROR(LOG_HEADER"error:camera image remap failed. camno=%d",i);
			}else{
				remap_img_bright = remap_img_filter.response.img;
				
				//ROS_INFO(LOG_HEADER"remap end. camno=%d",i);
				
				if(drawCameraOriginCrossFlg && i == 0){
					std::vector<float> matK;
					if( ! nh->getParam(PRM_CAM_CALIB_MAT_K_LIST[i],matK)){
						ROS_ERROR(LOG_HEADER"error:camera calib matrix get failed.");
					}else if( matK.size() != 9 ){
						ROS_ERROR(LOG_HEADER"error:camera calib matrix K is wrong.");
					}
					
					cv::Point pos(matK[2],matK[5]);
					pub_rects[i].publish(drawCameraOriginCross(remap_img_bright,pos));
				}else{
					pub_rects[i].publish(remap_img_bright);
				}
				pub_rects1[i].publish(remap_img_bright);
			}
		}
		{
			//remap-dark
			rovi::ImageFilter remap_img_filter;
			remap_img_filter.request.img = ros_imgs_darks[i];
			//ROS_INFO(LOG_HEADER"remap start. camno=%d",i);
			if( ! svc_remap[i].call(remap_img_filter) ){
				ROS_ERROR(LOG_HEADER"error:camera image remap failed. camno=%d",i);
			}else{
				//ROS_INFO(LOG_HEADER"remap end. camno=%d",i);
				remap_img_dark = remap_img_filter.response.img;
				pub_rects0[i].publish(remap_img_dark);
			}
		}
		//diff
		const sensor_msgs::Image diff_img = to_diff_img(remap_img_dark,remap_img_bright);
		pub_diffs[i].publish(diff_img);
	}
	
}

void on_pattern_image_received(const bool result,const int proc_tm,const std::vector<camera::ycam3d::CameraImage> &imgs_l,const std::vector<camera::ycam3d::CameraImage> &imgs_r,const bool timeout){
	ROS_INFO(LOG_HEADER"pattern image recevied. result=%s, proc_tm=%d ms, timeout=%d, imgs_l: num=%d, imgs_r: num=%d",
		(result?"OK":"NG"), proc_tm, timeout, (int)imgs_l.size(),(int)imgs_r.size());
	
	ElapsedTimer tmr;
	tmr.restart();
	
	if( ! result ){
		ROS_ERROR(LOG_HEADER"error:pattern capture failed.");
	}else{
		ptn_imgs_l = imgs_l;
		ptn_imgs_r = imgs_r;
	}
	if( timeout ){
		publish_string(pub_error,"Image streaming timeout");
	}
	//ROS_INFO(LOG_HEADER"elapsed tm=%d",tmr.elapsed_ms());
	
	ptn_capt_wait_cv.notify_one();
	// ********** ptn_capt_wait_cv NOTIFY **********
}


bool exec_point_cloud_generation(std_srvs::TriggerRequest &req, std_srvs::TriggerResponse &res){
	ROS_INFO("exec_point_cloud_generation");
	ElapsedTimer tmr;

	if( ! pc_gen_mutex.try_lock_for(std::chrono::seconds(0)) ){
		res.success = false;
		ROS_WARN(LOG_HEADER"genpc is busy");
		return true;
	}
	ptn_imgs_l.clear();
	ptn_imgs_r.clear();
	
	ROS_INFO("point cloud generation start.");
	bool result = false;
	std::lock_guard<std::timed_mutex> locker(pc_gen_mutex,std::adopt_lock);
	
	std::stringstream  res_msg_str;
	/*
	if( pre_ycam_mode != Mode_StandBy){
		ROS_WARN(LOG_HEADER"mode is not standby");
		
	}else */if( ! camera_ptr || ! camera_ptr->is_open() ){
		ROS_ERROR(LOG_HEADER"camera is null");
		
	} else {
		std::unique_lock<std::mutex> lock(ptn_capt_wait_mutex);
		if( ! camera_ptr->capture_pattern() ){
			ROS_ERROR(LOG_HEADER"pattern catpture failed.");
			
		}else{
			ptn_capt_wait_cv.wait(lock);
			// ********** ptn_capt_wait_cv WAIT **********
			
			if( ptn_imgs_l.size() != ptn_imgs_l.size() || camera::ycam3d::PATTERN_CAPTURE_NUM != ptn_imgs_l.size() ){
				ROS_ERROR(LOG_HEADER"pattern capture num is different.");
				
			}else{
				std::vector<sensor_msgs::Image> ros_ptn_imgs_l;
				std::vector<sensor_msgs::Image> ros_ptn_imgs_r;
				
				ros_ptn_imgs_l.assign(camera::ycam3d::PATTERN_CAPTURE_NUM,sensor_msgs::Image());
				ros_ptn_imgs_r.assign(camera::ycam3d::PATTERN_CAPTURE_NUM,sensor_msgs::Image());
				
				bool all_recevied = true;
				for( int i = 0 ; i < camera::ycam3d::PATTERN_CAPTURE_NUM ; ++i ){
					
					if( ! ptn_imgs_l.at(i).to_ros_img(ros_ptn_imgs_l[i],FRAME_ID) ){
						ROS_ERROR(LOG_HEADER"left pattern image is invalid. no=%d",i);
						all_recevied=false;
						break;
						
					}else if( ! ptn_imgs_r.at(i).to_ros_img(ros_ptn_imgs_r[i],FRAME_ID) ){
						ROS_ERROR(LOG_HEADER"right pattern image is invalid. no=%d",i);
						all_recevied=false;
						break;
						
					}
				}
				if(  ! all_recevied ){
					ROS_ERROR(LOG_HEADER"pattern image receive failed. elapsed=%d ms", tmr.elapsed_ms());
					
				}else{
					ROS_INFO(LOG_HEADER"all pattern image received. elapsed=%d ms", tmr.elapsed_ms());
					
					rovi::GenPC genpc_msg;
					genpc_msg.request.imgL = ros_ptn_imgs_l;
					genpc_msg.request.imgR = ros_ptn_imgs_r;
					
					if( ! svc_genpc.call(genpc_msg) ){
						ROS_ERROR(LOG_HEADER"genpc exec failed. elapsed=%d ms", tmr.elapsed_ms());
						exit(-1);
					}else{
						res_msg_str << ros_ptn_imgs_l.size() << " images scan complete. Generated PointCloud Count="
						<< genpc_msg.response.pc_cnt;
						
						publish_int32(pub_pcount,genpc_msg.response.pc_cnt);
						if( genpc_msg.response.pc_cnt < 1000 ){
							ROS_WARN(LOG_HEADER"The number of point clouds is small. count=%d",genpc_msg.response.pc_cnt);
						}
						result=true;
					}
					
					
					std::vector<sensor_msgs::Image> ros_imgs[2]={ros_ptn_imgs_l,ros_ptn_imgs_r};
					std::vector<camera::ycam3d::CameraImage> ptn_imgs[2]={ptn_imgs_l,ptn_imgs_r};
					
					ElapsedTimer tmr_remap;
					
					//�摜��z�M�����
					for( int camno = 0 ; camno < 2 ; ++camno ){
						pub_img_raws[camno].publish(ros_imgs[camno][1]);
						
						sensor_msgs::Image remap_ros_img_ptn_0;
						{
							rovi::ImageFilter remap_img_filter;
							remap_img_filter.request.img = ros_imgs[camno][0];
							if( ! svc_remap[camno].call(remap_img_filter) ){
								ROS_ERROR(LOG_HEADER"error:camera image remap failed. camno=%d, ptn=0",camno);
							}else{
								remap_ros_img_ptn_0 = remap_img_filter.response.img;
							}
							pub_rects0[camno].publish(remap_ros_img_ptn_0);
							
							//char path[256];
							//sprintf(path,"/tmp/remap_%d_0_A.pgm",camno);
							//cv::Mat img = cv_bridge::toCvCopy(ros_imgs[camno][0], sensor_msgs::image_encodings::MONO8)->image;
							//cv::imwrite(path,img);
							//
							//sprintf(path,"/tmp/remap_%d_0_B.pgm",camno);
							//img = cv_bridge::toCvCopy(remap_ros_img_ptn_0, sensor_msgs::image_encodings::MONO8)->image;
							//cv::imwrite(path,img);
						}
						
						sensor_msgs::Image remap_ros_img_ptn_1;
						{
							rovi::ImageFilter remap_img_filter;
							remap_img_filter.request.img = ros_imgs[camno][1];
							if( ! svc_remap[camno].call(remap_img_filter) ){
								ROS_ERROR(LOG_HEADER"error:camera image remap failed. camno=%d, ptn=1",camno);
							}else{
								remap_ros_img_ptn_1 = remap_img_filter.response.img;
							}
							pub_rects1[camno].publish(remap_ros_img_ptn_1);
							pub_rects[camno].publish(remap_ros_img_ptn_1);
							
							//char path[256];
							//sprintf(path,"/tmp/remap_%d_1_A.pgm",camno);
							//cv::Mat img = cv_bridge::toCvCopy(ros_imgs[camno][1], sensor_msgs::image_encodings::MONO8)->image;
							//cv::imwrite(path,img);
							//
							//sprintf(path,"/tmp/remap_%d_1_B.pgm",camno);
							//img = cv_bridge::toCvCopy(remap_ros_img_ptn_1, sensor_msgs::image_encodings::MONO8)->image;
							//cv::imwrite(path,img);
						}
						{
							sensor_msgs::Image diff_img = to_diff_img(remap_ros_img_ptn_0,remap_ros_img_ptn_1);
							pub_diffs[camno].publish(diff_img);
						}
						/*
						{
							
							sensor_msgs::Image diff_img;
							if( remap_ros_img_ptn_0.data.empty() ||
								remap_ros_img_ptn_0.data.size() != remap_ros_img_ptn_1.data.size()
							){
								ROS_ERROR(LOG_HEADER"error:remapped camera image is wrong. camno=%d",camno);
								
							}else{
								diff_img = remap_ros_img_ptn_0;
								for ( int n=0; n < remap_ros_img_ptn_0.data.size(); n++ ) {
									int val = remap_ros_img_ptn_1.data[n] - remap_ros_img_ptn_0.data[n];
									if (val < 1) { val = 0; }
									else if (val>255) { val=255; }
									
									diff_img.data[n] = val;
								}
							}
							
							pub_diffs[camno].publish(diff_img);
							
							//char path[256];
							//sprintf(path,"/tmp/remap_%d_diff.pgm",camno);
							//cv::Mat img = cv_bridge::toCvCopy(diff_img, sensor_msgs::image_encodings::MONO8)->image;
							//cv::imwrite(path,img);
						}*/
					}
					
				}
			}
		}
	}
	
	publish_bool(pub_Y1,result);
	
	res.success = true;
	res.message = res_msg_str.str();
	
	ROS_INFO(LOG_HEADER"point cloud generation finished. result=%d tmr=%d ms", result,  tmr.elapsed_ms());
	// ********** pc_gen_mutex UNLOCKED **********
	
	
	return true;
}


void exec_point_cloud_generation_sub(const std_msgs::Bool::ConstPtr &req){
	
	if( ! camera_ptr || ! camera_ptr->is_open() ){
		ROS_ERROR(LOG_HEADER"camera is not open");
		publish_bool(pub_Y1,false);
		return;
	}
	
	std_srvs::TriggerRequest trig_req;
	std_srvs::TriggerResponse trig_res;
	bool ret=exec_point_cloud_generation(trig_req,trig_res);
	
}
	
//============================================= �������O���  end  =============================================
}


int main(int argc, char **argv)
{
	ros::init(argc, argv, "ycam3d_node");
	ros::NodeHandle n;
	nh = &n;
	
	ROS_INFO(LOG_HEADER"initialization start.");
	if( ! init() ){
		ROS_ERROR(LOG_HEADER"error: initialization failed.");
		return 1;
	}
	ROS_INFO(LOG_HEADER"initialization finished");
	
	//camera initialization
	ROS_INFO(LOG_HEADER"camera: size= %d x %d, resolution=%s", cam_width, cam_height, camera_res.c_str());
	ROS_INFO(LOG_HEADER"ycam3d: mode=%d, cycle=%d Hz", pre_ycam_mode, cur_mode_mon_cyc);
	
	camera_ptr.reset(new CameraYCAM3D());
	
	if( ! camera_ptr->init(camera_res) ){
		ROS_ERROR(LOG_HEADER"error: camera initialization failed.");
		return 1;
	}
	
	camera_ptr->set_callback_camera_open_finished(on_camera_open_finished);
	camera_ptr->set_callback_camera_disconnect(on_camera_disconnect);
	camera_ptr->set_callback_camera_closed(on_camera_closed);
	camera_ptr->set_callback_capture_img_received(on_capture_image_received);
	camera_ptr->set_callback_pattern_img_received(on_pattern_image_received);
	
	//publishers
	pub_img_raws[0] = n.advertise<sensor_msgs::Image>("left/image_raw", 1);
	pub_img_raws[1] = n.advertise<sensor_msgs::Image>("right/image_raw", 1);
	pub_Y1 = n.advertise<std_msgs::Bool>("Y1", 1);
	pub_pcount = n.advertise<std_msgs::Int32>("pcount", 1);
	pub_stat = n.advertise<std_msgs::Bool>("stat", 1);
	pub_error = n.advertise<std_msgs::String>("error", 1);
	pub_info  = n.advertise<std_msgs::String>("message", 1);
	
	pub_rects[0] = n.advertise<sensor_msgs::Image>("left/image_rect", 1);
	pub_rects0[0] = n.advertise<sensor_msgs::Image>("left/image_rect0", 1);
	pub_rects1[0] = n.advertise<sensor_msgs::Image>("left/image_rect1", 1);
	pub_diffs[0] = n.advertise<sensor_msgs::Image>("left/diff_rect", 1);
	pub_views[0] = n.advertise<sensor_msgs::Image>("left/view",1);
	
	pub_rects[1] = n.advertise<sensor_msgs::Image>("right/image_rect", 1);
	pub_rects0[1] = n.advertise<sensor_msgs::Image>("right/image_rect0", 1);
	pub_rects1[1] = n.advertise<sensor_msgs::Image>("right/image_rect1", 1);
	pub_diffs[1] = n.advertise<sensor_msgs::Image>("right/diff_rect", 1);
	pub_views[1] = n.advertise<sensor_msgs::Image>("right/view",1);
	
	//service servers
	const ros::ServiceServer svs1 = n.advertiseService("pshift_genpc", exec_point_cloud_generation);
	
	//service clients
	svc_genpc = n.serviceClient<rovi::GenPC>("genpc");
	svc_remap[0] = n.serviceClient<rovi::ImageFilter>("left/remap");
	svc_remap[1] = n.serviceClient<rovi::ImageFilter>("right/remap");
	
	//subscribers
	const ros::Subscriber sub1 = n.subscribe<std_msgs::Bool>("X1", 1, exec_point_cloud_generation_sub);
	
	//timers
	mode_mon_timer = n.createTimer(ros::Duration(1/(float)cur_mode_mon_cyc), mode_monitor_task);
	cam_open_mon_timer = n.createTimer(ros::Duration(1), cam_open_monitor_task);
	
	camera_ptr->set_callback_auto_con_limit_exceeded([&](){
		g_node_exit_flg = 1;
	});
	camera_ptr->start_auto_connect(cam_ipaddr);
	
	bool activeDelyMonitor=false;
	if( get_param<bool>(PRM_NW_DELAY_MON_ENABLED,false) ){
		const int delayMonInterval= get_param<int>(PRM_NW_DELAY_MON_INTERVAL,1);
		const int delayMonTimeout= get_param<int>(PRM_NW_DELAY_MON_TIMEOUT,500);
		const bool delayMonIgnUpdFail = get_param<bool>(PRM_NW_DELAY_MON_IGN_UPD_FAIL,false);
		ROS_INFO("delay monistor start. interval=%d",delayMonInterval);
		camera_ptr->start_nw_delay_monitor_task(delayMonInterval,delayMonTimeout,[&](){
			ROS_ERROR(LOG_HEADER"network delay occurred !!!");
			g_node_exit_flg = 1;
		},delayMonIgnUpdFail);
		activeDelyMonitor = true;
	}
	//ros::spin();
	while( ros::ok() && g_node_exit_flg == 0 ){
		ros::spinOnce();
	}
	
	mode_mon_timer.stop();
	ROS_INFO(LOG_HEADER"mode monitor timer stopped.");
	
	cam_open_mon_timer.stop();
	ROS_INFO(LOG_HEADER"camera open monitor timer stopped.");
	
	//todo:******�摜�]���X���b�h�̏I���҂�
	ROS_INFO(LOG_HEADER"image transfer task wait start.");

	ROS_INFO(LOG_HEADER"image transfer task wait finished.");
	
	if( camera_ptr ){
		ROS_INFO(LOG_HEADER"camera close start.");
		camera_ptr.reset();
		ROS_INFO(LOG_HEADER"camera close finished.");
	}
	return 0;
}
