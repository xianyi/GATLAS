//    Copyright 2010 Chris Jang
//
//    This file is part of GATLAS.
//
//    GATLAS is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Lesser General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    GATLAS is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public License
//    along with GATLAS.  If not, see <http://www.gnu.org/licenses/>.

#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include "GatlasAppUtil.hpp"
#include "GatlasBenchmark.hpp"

#include "KernelProbeAutoVectorize.hpp"
#include "KernelFile.hpp"

#include "using_namespace"

using namespace std;

bool parseOpts(int argc, char *argv[],
               string& device,
               int& M,
               int& N,
               int& K,
               int& groupSize,
               int& blockHeight,
               int& extraParam,
               size_t& numberTrials,
               int& topN,
               bool& nestedOptimization,
               bool& transposeA,
               bool& transposeB,
               bool& busTransferToDevice,
               bool& busTransferFromDevice,
               bool& paranoidCheck,
               bool& vectorAttributeHint,
               bool& printDebug) {
    int opt;
    while ((opt = getopt(argc, argv, "heabsrpvzd:m:n:k:g:y:x:t:w:")) != -1) {
        switch (opt) {
            case ('h') :
                cerr << "usage: " << argv[0]
                     << " -d cpu|gpu|acc|cpuX|gpuX|accX -n N [-m M -k K]"
                        " [-g groupSize [-y blockHeight [-x extraParam]]]"
                        " [-t numberTrials]"
                        " [-w topN]"
                        " [-e] [-a] [-b] [-s] [-r] [-p] [-v] [-z] [-h]" << endl
                     << "\t-d cpu, gpu or accelerator device, optional X is the device number" << endl
                     << "\t-m matrix dimension M" << endl
                     << "\t-n matrix dimension N" << endl
                     << "\t-k matrix dimension K" << endl
                     << "\t-g work item group width and height" << endl
                     << "\t-y inner blocking height" << endl
                     << "\t-x extra parameter" << endl
                     << "\t-t number of trials (default is 1)" << endl
                     << "\t-w keep topN (groupSize, blockHeight) combinations" << endl
                     << "\t-e use faster nested optimization (default no)" << endl
                     << "\t-a transpose A (default no)" << endl
                     << "\t-b transpose B (default no)" << endl
                     << "\t-s include PCIe bus data transfer to device in timing (default no)" << endl
                     << "\t-r include PCIe bus data transfer from device in timing (default no)" << endl
                     << "\t-p paranoid output matrix check (default no)" << endl
                     << "\t-v disable kernel vector attribute hint (default enabled)" << endl
                     << "\t-z print matrix output (default no)" << endl
                     << "\t-h help" << endl;
                exit(1);
            case ('d') : device = optarg; break;
            case ('m') : M = atoi(optarg); break;
            case ('n') : N = atoi(optarg); break;
            case ('k') : K = atoi(optarg); break;
            case ('g') : groupSize = atoi(optarg); break;
            case ('y') : blockHeight = atoi(optarg); break;
            case ('x') : extraParam = atoi(optarg); break;
            case ('t') : numberTrials = atoi(optarg); break;
            case ('w') : topN = atoi(optarg); break;
            case ('e') : nestedOptimization = true; break;
            case ('a') : transposeA = true; break;
            case ('b') : transposeB = true; break;
            case ('s') : busTransferToDevice = true; break;
            case ('r') : busTransferFromDevice = true; break;
            case ('p') : paranoidCheck = true; break;
            case ('v') : vectorAttributeHint = false; break;
            case ('z') : printDebug = true; break;
        }
    }

    // minimal validation of options
    const size_t VL = VECTOR_LENGTH_MACRO ;
    bool rc = true;
    if (0 != device.find("cpu") && 0 != device.find("gpu") & 0 != device.find("acc")) {
        cerr << "error: invalid device " << device << endl;
        rc = false;
    }
    if (-1 == N) {
        cerr << "error: matrix dimension N must be specified" << endl;
        rc = false;
    } else {
        if (-1 == M && -1 == K) {
            M = K = N;
        } else {
            if ( (-1 != M && -1 == K) || (-1 == M && -1 != K) ) {
                cerr << "error: all matrix dimensions M, N and K must be specified" << endl;
                rc = false;
            }
        }
    }
    if (0 != N % VL) {
        cerr << "error: matrix dimension N must be multiple of " << VL << endl;
        rc = false;
    }
    if (0 != M % VL) {
        cerr << "error: matrix dimension M must be multiple of " << VL << endl;
        rc = false;
    }
    if (0 != K % VL) {
        cerr << "error: matrix dimension K must be multiple of " << VL << endl;
        rc = false;
    }
    if (-1 == groupSize && -1 != blockHeight) {
        cerr << "error: group size must be specified with block height" << endl;
        rc = false;
    }
    if (-1 == groupSize && -1 == blockHeight && -1 != extraParam) {
        cerr << "error: group size and block height must be specified with extra parameter" << endl;
        rc = false;
    }
    if (-1 != groupSize && (groupSize < 1 || groupSize > 16)) {
        cerr << "error: work item group size must be a number from 1 to 16 inclusive" << endl;
        rc = false;
    }
    if (transposeA && -1 != blockHeight && (0 != blockHeight % VL) ) {
        cerr << "error: inner blocking height must be multiple of " << VL << " when matrix A is transposed" << endl;
        rc = false;
    }
    if (-1 != blockHeight && blockHeight < VL) {
        cerr << "error: invalid inner blocking height" << endl;
        rc = false;
    }

    // doesn't really make sense to specify blocking with nested optimization
    if (nestedOptimization && (-1 != groupSize || -1 != blockHeight || -1 != extraParam)) {
        cerr << "error: nested optimization will find optimal blocking" << endl;
        rc = false;
    }

    return rc;
}

vector< vector<size_t> > getParams(OCLApp& oclApp,
                                   KERNEL_CLASS_MACRO < SCALAR_MACRO , VECTOR_LENGTH_MACRO > & kernel,
                                   const size_t M, const size_t N, const size_t K,
                                   const bool transposeA, const bool transposeB,
                                   const size_t groupSize, const size_t blockHeight, const size_t extraParam,
                                   const int loopOrder)
{
    vector< vector<size_t> > pargs;
    vector<size_t> a;

    kernel.setMatrixDimensions(M, N, K);
    kernel.setDataLayout(transposeA, transposeB);
    if (-1 != groupSize) kernel.setWorkGroup(groupSize);
    if (-1 != blockHeight) kernel.setInnerBlocking(blockHeight, VECTOR_LENGTH_MACRO );
    if (-1 != extraParam) kernel.setExtraParameter(extraParam);

    // all parameters
    if (-1 != groupSize && -1 != blockHeight && -1 != extraParam) {
        if (kernel.getParams(a)) pargs.push_back(a);
    } else {
        // extra parameter is free
        if (-1 != groupSize && -1 != blockHeight) {
            for (size_t xp = 0; xp < kernel.totalVariations(); xp++) {
                kernel.setExtraParameter(xp);
                if (kernel.getParams(a)) pargs.push_back(a);
            }

        // inner blocking and extra parameter are free
        } else if (-1 != groupSize) {
            for (size_t bh = VECTOR_LENGTH_MACRO ; bh <= MAX_BLOCK_HEIGHT_MACRO; bh++) {
                kernel.setInnerBlocking(bh, VECTOR_LENGTH_MACRO );
                for (size_t xp = 0; xp < kernel.totalVariations(); xp++) {
                    kernel.setExtraParameter(xp);
                    if (kernel.getParams(a)) pargs.push_back(a);
                }
            }

        // work group size, inner block and extra parameter are free
        } else {
            // maximum value of group size
            const size_t maxPossibleGroupSize = sqrt(oclApp.maxWorkGroupSize());
            const size_t maxGroupSize = MAX_GROUP_SIZE_MACRO < maxPossibleGroupSize
                                            ? MAX_GROUP_SIZE_MACRO
                                            : maxPossibleGroupSize;

            // largest valid group size for problem dimensions
            for (size_t wg = maxGroupSize; wg > 8; wg--) {
                kernel.setWorkGroup(wg);
                bool notEmpty = false;
                for (size_t bh = VECTOR_LENGTH_MACRO ; bh <= MAX_BLOCK_HEIGHT_MACRO; bh++) {
                    kernel.setInnerBlocking(bh, VECTOR_LENGTH_MACRO );
                    if (-1 == loopOrder) {
                        for (size_t xp = 0; xp < kernel.totalVariations(); xp++) {
                            kernel.setExtraParameter(xp);
                            if (kernel.getParams(a)) {
                                pargs.push_back(a);
                                notEmpty = true;
                            }
                        }
                    } else {
                        kernel.setExtraParameter(0);
                        if (kernel.getParams(a)) {
                            pargs.push_back(a);
                            notEmpty = true;
                        }
                    }
                }
                if (notEmpty) break;
            }

            // work group size of 64, same as wavefront on 5870
            kernel.setWorkGroup(8);
            for (size_t bh = VECTOR_LENGTH_MACRO ; bh <= MAX_BLOCK_HEIGHT_MACRO; bh++) {
                kernel.setInnerBlocking(bh, VECTOR_LENGTH_MACRO );
                if (-1 == loopOrder) {
                    for (size_t xp = 0; xp < kernel.totalVariations(); xp++) {
                        kernel.setExtraParameter(xp);
                        if (kernel.getParams(a)) pargs.push_back(a);
                    }
                } else {
                    kernel.setExtraParameter(0);
                    if (kernel.getParams(a)) pargs.push_back(a);
                }
            }
        }
    }

    return pargs;
}

// return number of benchmarked kernels that were ok
size_t mainLoop(KernelInterface& kernel,
                Bench& bench,
                const vector< vector<size_t> >& pargs,
                vector<bool>& pargsOk,
                vector<double>& pargsAverage,
                const size_t numberTrials,
                const size_t topN,
                const bool busTransferToDevice,
                const bool busTransferFromDevice,
                const bool printDebug,
                const bool paranoidCheck) {

    size_t goodKernelCount = 0;

    vector<size_t> pargsTime;
    vector<size_t> pargsFlops;
    vector<double> pargsVariance;
    vector< vector<size_t> > pargsExtraDetail;

    for (size_t i = 0; i < pargs.size(); i++) {
        pargsTime.push_back(0);
        pargsFlops.push_back(0);
        pargsVariance.push_back(0);
        pargsExtraDetail.push_back(vector<size_t>());
    }

    // paranoid check
    if (paranoidCheck) kernel.paranoidCheck();

    // repeat main loop for number of trials
    for (size_t k = 0; k < numberTrials; k++) {

        const bool dummyRun = (0 == k);

        goodKernelCount = AppUtil::benchLoop(k,
                                             kernel,
                                             bench,
                                             pargs,
                                             pargsOk,
                                             pargsTime,
                                             pargsFlops,
                                             pargsAverage,
                                             pargsVariance,
                                             pargsExtraDetail,
                                             busTransferToDevice,
                                             busTransferFromDevice,
                                             dummyRun,
                                             printDebug);

        cout << endl;

        // prune to top N parameter combinations
        // parameter combinations with the same time are treated together
        if (-1 != topN) AppUtil::markBench(topN, pargsOk, pargsTime);
    }

    AppUtil::printBench(numberTrials,
                        pargs,
                        pargsOk,
                        pargsTime,
                        pargsAverage,
                        pargsVariance,
                        pargsExtraDetail);

    return goodKernelCount;
}

size_t mainLoop(KernelInterface& kernel,
                Bench& bench,
                const vector< vector<size_t> >& pargs,
                const size_t numberTrials,
                const size_t topN,
                const bool busTransferToDevice,
                const bool busTransferFromDevice,
                const bool printDebug,
                const bool paranoidCheck) {

    vector<bool> pargsOk;
    vector<double> pargsAverage;

    for (size_t i = 0; i < pargs.size(); i++) {
        pargsOk.push_back(true);
        pargsAverage.push_back(0);
    }

    return mainLoop(kernel,
                    bench,
                    pargs,
                    pargsOk,
                    pargsAverage,
                    numberTrials,
                    topN,
                    busTransferToDevice,
                    busTransferFromDevice,
                    printDebug,
                    paranoidCheck);
}

int main(int argc, char *argv[])
{
    string device = "<unspecified>";
    int M = -1, N = -1, K = -1;
    int groupSize = -1, blockHeight = -1, extraParam = -1;
    size_t numberTrials = 1;
    int topN = -1;
    bool nestedOptimization = false;
    bool transposeA = false, transposeB = false;
    bool busTransferToDevice = false, busTransferFromDevice = false;
    bool paranoidCheck = false;
    bool vectorAttributeHint = true;
    bool printDebug = false;

    if (!parseOpts(argc, argv,
                   device,
                   M, N, K,
                   groupSize, blockHeight, extraParam,
                   numberTrials,
                   topN,
                   nestedOptimization,
                   transposeA, transposeB,
                   busTransferToDevice, busTransferFromDevice,
                   paranoidCheck,
                   vectorAttributeHint,
                   printDebug))
        exit(1);

    OCLBase oclBase;
    const size_t device_index = AppUtil::getDeviceIndex(oclBase, device);
    OCLApp oclApp(oclBase, device_index);

    KERNEL_CLASS_MACRO < SCALAR_MACRO , VECTOR_LENGTH_MACRO > kernel( GEMM_MACRO );
    Bench bench(oclApp, kernel);

    // kernel vector attribute hint?
    kernel.setUseAttrAutoVec(vectorAttributeHint);

    vector< vector<size_t> > pargs = getParams(oclApp,
                                               kernel,
                                               M, N, K,
                                               transposeA, transposeB,
                                               groupSize, blockHeight, extraParam,
                                               -1);
                                               //(nestedOptimization ? 0 : -1));

    vector<bool> pargsOk;
    vector<double> pargsAverage;
    for (size_t i = 0; i < pargs.size(); i++) {
        pargsOk.push_back(true);
        pargsAverage.push_back(0);
    }

    mainLoop(kernel,
             bench,
             pargs,
             pargsOk,
             pargsAverage,
             nestedOptimization ? 1 : numberTrials,
             topN,
             busTransferToDevice,
             busTransferFromDevice,
             printDebug,
             paranoidCheck);

    if (! nestedOptimization) return 0; // one full pass only

    // nested optimization needs second pass
    cout << endl << "*** nested optimization second pass ***" << endl;

    // find inner and outer blocking of the fastest kernel
    const size_t bestIndex = AppUtil::rankBench(0, pargsOk, pargsAverage);
    kernel.setParams(pargs[bestIndex]);
    const size_t bestGroupSize = kernel.groupSize();
    const size_t bestBlockHeight = kernel.blockHeight();

    pargs = getParams(oclApp,
                      kernel,
                      M, N, K,
                      transposeA, transposeB,
                      bestGroupSize, bestBlockHeight, extraParam,
                      -1);

    mainLoop(kernel,
             bench,
             pargs,
             numberTrials,
             topN,
             busTransferToDevice,
             busTransferFromDevice,
             printDebug,
             paranoidCheck);

    return 0;
}
