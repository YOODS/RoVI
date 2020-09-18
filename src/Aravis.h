#pragma once
#include <arv.h>
#include <stdint.h>
#include <string>

#include "YCAM3D.h"


//2020/09/15 add by hato -------------------- start --------------------
namespace aravis{
namespace ycam3d{
	constexpr int CAM_EXPOSURE_TIME_DEFAULT   = 8300;
	constexpr int CAM_EXPOSURE_TIME_MAX       = 30000;
	constexpr int CAM_EXPOSURE_TIME_MIN       = 1000;
	
	constexpr int CAM_DIGITAL_GAIN_DEFAULT   = 0;
	constexpr int CAM_DIGITAL_GAIN_MAX       = 255;
	constexpr int CAM_DIGITAL_GAIN_MIN       = 0;
	
	constexpr int CAM_ANALOG_GAIN_DEFAULT   = 0;
	constexpr int CAM_ANALOG_GAIN_MAX       = 6;
	constexpr int CAM_ANALOG_GAIN_MIN       = 0;
	
	constexpr int PROJ_EXPOSURE_TIME_DEFAULT = 8333;
	constexpr int PROJ_EXPOSURE_TIME_MIN     = 0;      //?
	constexpr int PROJ_EXPOSURE_TIME_MAX     = 9999999;//?
	
	constexpr int PROJ_BRIGHTNESS_DEFAULT = 100;   //?
	constexpr int PROJ_BRIGHTNESS_MIN     = 0;     //?
	constexpr int PROJ_BRIGHTNESS_MAX     = 9999;  //?
	
	constexpr int PROJ_FLASH_INTERVAL_DEFAULT = 10;
	constexpr int PROJ_FLASH_INTERVAL_MIN = 0;  //?
	constexpr int PROJ_FLASH_INTERVAL_MAX = 999;//?
	
	constexpr int PATTERN_CAPTURE_NUM  = PHSFT_CAP_NUM;
	
	struct ExposureTimeSet {
		const int cam_exposure_tm;
		const int proj_exposure_tm;
		
		ExposureTimeSet(const int a_cam_expsr_tm,const int a_proj_expsr_tm):
			cam_exposure_tm(a_cam_expsr_tm),proj_exposure_tm(a_proj_expsr_tm)
		{
		}
	};
	constexpr int EXPOSURE_TIME_SET_DEFAULT = 0;
	extern const ExposureTimeSet EXPOSURE_TIME_SET_LIST[];
	extern const int EXPOSURE_TIME_SET_SIZE;
}
}
//2020/09/15 add by hato --------------------  end  --------------------


struct OnRecvImage {
	/**
	* @brief 画像データコールバック関数型
	* @param[out] camno 0～
	* @param[out] frmidx 0～12
	* @param[out] width 幅
	* @param[out] height 高さ
	* @param[out] color(0:mono,1:color)
	* @param[out] mem データアドレス
	*/
	virtual void operator()(int camno, int frmidx, int width, int height, int color, void *mem) = 0;
};

struct OnLostCamera {
	virtual void operator()(int camno) = 0;
};

class Aravis
{
	static int static_camno_;
	int camno_;
	int resolution_;
	int ncam_;
	ArvCamera *camera_;
	ArvDevice *device_;
	ArvStream *stream_;
	int payload_;		//buffer size (2 cams)
	int width_,height_;	//whole image size (2 cams)
	int color_;
	int frame_index_;
	pthread_cond_t cap_cond_;
	pthread_mutex_t cap_mutex_;

	ArvBufferStatus buffer_status_;
	bool lost_;
	char name_[64];
	int64_t packet_delay_;
	uint16_t version_;
	YCAM_TRIG trigger_mode_;
	
	
	//2020/09/16 add by hato -------------------- start --------------------
	int m_exposure_tm_lv;
	//2020/09/16 add by hato --------------------  end  --------------------

	//Aravis control
	static void on_new_buffer(ArvStream *stream, void *arg);
	static void on_control_lost(ArvGvDevice *gv_device, void *arg);

	//PROJ
	bool uart_write(const char *cmd);
	bool uart_write(char command, int data);
	bool uart_write(char command, const char *data);
	void uart_flush();
	int projector_value(const char *key_str, std::string *str = 0);	//診断メッセージから値を取得
	bool projector_wait();

	//YCAM
	uint32_t reg_read(uint64_t reg);
	bool reg_write(uint64_t reg, int value);
	
	//Genicam
	std::string get_node_str(const char *feature);
	int64_t get_node_int(const char *feature);
	bool set_node_int(const char *feature, int64_t value);
	double get_node_float(const char *feature);
	std::string get_description(const char *feature);

	//callback
	OnRecvImage *on_image_;
	uint8_t *out_;
	OnLostCamera *on_lost_;

public:
	Aravis(YCAM_RES res = YCAM_RES_SXGA, int ncam = 1);
	~Aravis();
	bool openCamera(const char *name=0, const int packet_size=0);	//name or ip address
	bool openStream(int nbuf=10);
	void destroy();
	bool capture(unsigned char *data, float timeout_sec=0.0f);
	//
	void setPacketDelay(int64_t delay);	//us
	int64_t packetDelay(){return packet_delay_;}
	//
	char *name(){return name_;}
	int frameSize(){return payload_;}
	void imageSize(int *width, int *height);
	int width(){return width_;}
	int height(){return height_;}
	int  bytesPerPixel(){payload_ / (width_ * height_);}
	int cameraNo()const { return camno_; }
	
	
	//2020/09/16 add by hato -------------------- start --------------------
	int get_exposure_tm_level()const;
	bool set_exposure_tm_level(const int val);
	//2020/09/16 add by hato --------------------  end  --------------------
	
	//
	int exposureTime();
	int gainA();
	int gainD();
	bool setExposureTime(int value);
	bool setGainA(int value);
	bool setGainD(int value);
	//
	bool isLost(){return lost_;}
	bool isAsync(){ return (VER_ACAP <= version_); }
	//
	YCAM_TRIG triggerMode(){ return trigger_mode_; }
	bool setTriggerMode(YCAM_TRIG tm);
	bool trigger(YCAM_PROJ_MODE mode);
	//
	bool setProjectorPattern(YCAM_PROJ_PTN ptn);

	bool setProjectorBrightness(int value);
	int projectorBrightness();

	bool setProjectorExposureTime(int value);
	int projectorExposureTime();

	bool setProjectorFlashInterval(int value);
	int projectorFlashInterval();

	//utilities
	std::string uart_dump();
	bool upload_camparam(YCAM_RES reso, YCAM_SIDE side, const char *yaml_path);

	/**
	* @brief 画像データ取得時のコールバック関数設定
	* @param[in] onRecvImage コールバック関数
	* @param[out] out 画像バッファアドレス(固定)
	*/
	void addCallbackImage(OnRecvImage *onRecvImage, uint8_t *out);
	/**
	* @brief カメラロスト時のコールバック関数設定
	* @param[in] onLost コールバック関数
	*/
	void addCallbackLost(OnLostCamera *onLost);
};
