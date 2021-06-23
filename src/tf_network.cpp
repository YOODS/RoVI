#include "ros/ros.h"
#include "sensor_msgs/Image.h"
#include <sstream>
#include <cv_bridge/cv_bridge.h>
#include "boost/bind.hpp"
#include <iostream>


static ros::Publisher *pubL, *pubR;


//�摜�������ĂRD���W��Ԃ�
void find_marker(sensor_msgs::Image buf, int label){

	std::cout << "enter callback" << std::endl;	
	/** �摜���� **/
	cv_bridge::CvImagePtr cv_ptr;	//OpenCV�p�̃|�C���^��p��
	cv_ptr = cv_bridge::toCvCopy(buf, sensor_msgs::image_encodings::BGR8);	//ROS����OpenCV�`���ɕϊ��Bcv_ptr->image��cv::Mat�t�H�[�}�b�g
	
	//�摜�̑O����
	cv::Mat image, gray_image, gaussian_image, thr_image, norm_img, g_img, normalized_int, normalized_image;
	cv::cvtColor(cv_ptr->image, gray_image, cv::COLOR_BGR2GRAY);	//�O���[�X�P�[���ϊ�
	cv::GaussianBlur(gray_image, gaussian_image, cv::Size(5, 5), 3, 3);	//������

	//��f�l�̐��K���i���ςƕW���΍��j
	cv::Scalar mean, stddev;
	cv::meanStdDev(gaussian_image, mean, stddev);
	int s = 90;	//�W���΍�
	int m = 100;
	int rows = gaussian_image.rows;
	int cols = gaussian_image.cols;
	cv::Mat mean_Mat = cv::Mat::ones(rows, cols, CV_32FC1) * mean[0];
	cv::Mat m_Mat = cv::Mat::ones(rows, cols, CV_32FC1) * m;
	gaussian_image.convertTo(g_img, CV_32FC1);
	norm_img = (g_img - mean_Mat) / stddev[0] * s + m_Mat;
	norm_img.convertTo(normalized_int, CV_32SC1);	// CV_8S��8bit(1byte) = char�^
	normalized_image = cv::Mat::ones(rows, cols, CV_8UC1);
	for (int i = 0; i < norm_img.rows; i++) {
		int* nintP = normalized_int.ptr<int>(i);
		uchar* nimgP = normalized_image.ptr<uchar>(i);
		for (int j = 0; j < norm_img.cols; j++) {
			int value = nintP[j];
			if (value < 0) {
				value = 0;
			}else if (value > 255) {
				value = 255;
			}
			nimgP[j] = value;
		}
	}

	//��l������
	cv::threshold(normalized_image, thr_image, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);	//��Â̓�l��
	
	//�֊s���o
	cv::Mat color_img;
	std::vector<std::vector<cv::Point>> contours;
	std::vector< cv::Vec4i > hierarchy;
	cv::findContours(thr_image, contours, hierarchy, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);	
	cv::cvtColor(normalized_image, color_img, cv::COLOR_GRAY2BGR);	//�O���[�X�P�[���摜��RBG�ɕϊ�

	//�֊s�̂����~�`�x�̍������̂����o���~�𐄒�
	int idx = 0, flag = 0;
	if (contours.size()) {
		for (; idx >= 0; idx = hierarchy[idx][0]) {
			drawContours(color_img, contours, idx, cv::Scalar(80, 244, 255), 2);	// i �Ԗڂ̗֊s��`���B�֊s�̐F�̓������C�G���[
			const std::vector<cv::Point>& c = contours[idx];
			double area = fabs(cv::contourArea(cv::Mat(c)));	//�֊s�ň͂܂ꂽ�ʐ�S
			double perimeter = cv::arcLength(c, true); //�֊s�̒���

			//�~�`�x(4��S / L^2)�̍����֊s�����o
			double circle_deg;
			float radius;
			cv::Point2f center;
			circle_deg = 4 * M_PI*area / pow(perimeter, 2.0);

			//�~�𐄒聕�~�ƒ��S���W��`��
			if (circle_deg > 0.8 && area > 100) {
				cv::minEnclosingCircle(c, center, radius);	//�ŏ��O�ډ~���v�Z
				cv::circle(color_img, center, radius, cv::Scalar(255, 0, 255), 2);	//�O�ډ~��`��
				cv::drawMarker(color_img, center, cv::Scalar(255, 0, 255));	//���S���W��`��
			}
		}
	}
	
	//���ʉ摜��ROS�p�`���ɕϊ�
	sensor_msgs::Image img;
	cv_ptr->image = color_img;
	cv_ptr->encoding="bgr8";	
	cv_ptr->toImageMsg(img);	//toImageMsg()��OpenCV����ros�̌^�ɕϊ�
	if (label==0){
		pubL->publish(img);
	}else{
		pubR->publish(img);
	}
	
	/** �RD���W�ւ̕ϊ� **/
	
}

void find_marker_L(sensor_msgs::Image buf){
	find_marker(buf, 0);
}
	
void find_marker_R(sensor_msgs::Image buf){
	find_marker(buf, 1);
}	
	

int main(int argc, char** argv){
	
	//�V�����m�[�h�ikidzania_node�j�̍쐬
	ros::init(argc, argv,"kidzania_node");
	
	//�m�[�h�ւ̃n���h���̍쐬�i�m�[�h�̏������j
	ros::NodeHandle n;
	
	//�g�s�b�N��sensor_msgs::Image�^�̉摜�𔭍s���鏀��
	ros::Publisher pL = n.advertise<sensor_msgs::Image>("kidzania/image_left_out", 1000);
	ros::Publisher pR = n.advertise<sensor_msgs::Image>("kidzania/image_right_out", 1000);
	pubL = &pL;
	pubR = &pR;
	
	//�g�s�b�N�ichatter�j��sensor_msgs::Image�^�̉摜����M�i�R�[���o�b�N�֐������j
	ros::Subscriber subL = n.subscribe("/rovi/left/image_rect", 1000, find_marker_L);
	ros::Subscriber subR = n.subscribe("/rovi/right/image_rect", 1000, find_marker_R);
	
	ros::spin();
	
	return 0;
}
