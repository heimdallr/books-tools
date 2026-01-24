//
//  canny.h
//  Canny Edge Detector
//
//  Created by Hasan Akgün on 21/03/14.
//  Modifed by Michael Wang on 10/03/17.
//  Software is released under GNU-GPL 2.0
//

#pragma once

#include <vector>

#include "CImg.h"

using namespace cimg_library;
using namespace std;

class Canny {
private:

    CImg<unsigned char> img; //Original Image
    CImg<unsigned char> gFiltered; // Gradient
    CImg<unsigned char> sFiltered; //Sobel Filtered
    CImg<float> angles; //Angle Map
    CImg<unsigned char> nonMaxSupped; // Non-maxima supp.
    CImg<unsigned char> thres; //Double threshold and final

    int    _gfs { 3 }, _thres_lo {20}, _thres_hi{40};
    double _g_sig{1.0};

public:

    //Constructor
    //@param const char* name : name of the image, path will be auto concatenated to "./output/name.bmp". 
    explicit Canny(CImg<unsigned char> img);

    vector< vector<double> > createFilter(int row, int col, double sigma_in); //Creates a gaussian filter

    void useFilter(CImg<unsigned char>, const vector< vector<double>>&); //Use some filter

    void sobel(); //Sobel filtering

    void nonMaxSupp(); //Non-maxima supp.
    
    void threshold(CImg<unsigned char>, int, int); //Double threshold and finalize picture

    //Main Process Function with different parameter setting.

    //@param
    //int gfs: gaussian filter size, odd number only!!!
    //double g_sig: gaussian sigma
    //int thres_lo: lower bound of double thresholding, should be less than higher bound, 0-255.
    //int thres_hi: uppoer bound of double thresholding, should be higher than lower bound, 0-255.

    //@return
    //CImg<unsigned char> final result of processed image.
	CImg<unsigned char> process(int gfs = 3, double g_sig = 1, int thres_lo = 20, int thres_hi = 40);
};
