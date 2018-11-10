/**
>HEADER
    Copyright (c) 2013, 2014, 2015, 2016 Rob Patro rob.patro@cs.stonybrook.edu

    This file is part of Salmon.

    Salmon is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Salmon is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Salmon.  If not, see <http://www.gnu.org/licenses/>.
<HEADER
**/

#include <random>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

// C++ string formatting library
#include "spdlog/fmt/fmt.h"

// Boost Includes
#include <boost/container/flat_map.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

// logger includes
#include "spdlog/spdlog.h"

#include "cereal/types/vector.hpp"

// utility includes
#include "nonstd/string_view.hpp"
#include "nonstd/optional.hpp"

//alevin include
#include "Filter.hpp"
#include "AlevinOpts.hpp"
#include "AlevinUtils.hpp"
#include "BarcodeModel.hpp"
#include "SingleCellProtocols.hpp"
#include "BarcodeGroup.hpp"
#include "tsl/array_map.h"

// salmon includes
#include "FastxParser.hpp"
#include "SalmonConfig.hpp"
#include "SalmonDefaults.hpp"
#include "SalmonOpts.hpp"
#include "ProgramOptionsGenerator.hpp"

using paired_parser_qual = fastx_parser::FastxParser<fastx_parser::ReadQualPair>;
using single_parser = fastx_parser::FastxParser<fastx_parser::ReadSeq>;

constexpr uint32_t miniBatchSize{5000};

/* ALEVIN DECLERATIONS*/
using bcEnd = BarcodeEnd;
namespace apt = alevin::protocols;
namespace aut = alevin::utils;

template <typename ProtocolT>
int alevinQuant(AlevinOpts<ProtocolT>& aopt,
                SalmonOpts& sopt,
                SoftMapT& barcodeMap,
                TrueBcsT& trueBarcodes,
                boost::program_options::parsed_options& orderedOptions,
                CFreqMapT& freqCounter,
                size_t numLowConfidentBarcode);

//colors for progress monitoring
const char RESET_COLOR[] = "\x1b[0m";
char green[] = "\x1b[30m";
char red[] = "\x1b[30m";

/*
  Parse through mate1 file Rapidly and counts the density of each barcode
 */
template <typename ProtocolT>
void densityCalculator(single_parser* parser,
                       AlevinOpts<ProtocolT>& aopt,
                       CFreqMapT& freqCounter,
                       std::atomic<uint64_t>& usedNumBarcodes,
                       std::atomic<uint64_t>& totNumBarcodes){
  size_t rangeSize{0};
  uint32_t index;

  auto rg = parser->getReadGroup();
  auto log = aopt.jointLog;

  uint64_t usedNumBarcodesLocal{0};
  uint64_t totNumBarcodesLocal{0};
  while (parser->refill(rg)) {
    rangeSize = rg.size();
    for (size_t i = 0; i < rangeSize; ++i) { // For all the read in this batch
      //Sexy Progress monitor
      ++totNumBarcodesLocal;
      if (not aopt.quiet and totNumBarcodesLocal % 500000 == 0) {
        fmt::print(stderr, "\r\r{}processed{} {} Million {}barcodes{}",
                   green, red, totNumBarcodesLocal/1000000, green, RESET_COLOR);
      }

      auto& rp = rg[i];
      std::string& seq = rp.seq;
      //if (aopt.protocol.end == bcEnd::THREE) {
      //  std::reverse(seq.begin(), seq.end());
      //}

      nonstd::optional<std::string> extractedBarcode = aut::extractBarcode(seq, aopt.protocol);
      bool seqOk = (extractedBarcode.has_value()) ? aut::sequenceCheck(*extractedBarcode) : false;
      if (not seqOk){
        continue;
      }
      freqCounter[*extractedBarcode] += 1;
      ++usedNumBarcodesLocal;
    }//end-for
  }//end-while
  totNumBarcodes += totNumBarcodesLocal;
  usedNumBarcodes += usedNumBarcodesLocal;
}

template <typename T>
std::vector<size_t> sort_indexes(const std::vector<T> &v) {
  // initialize original index locations
  std::vector<size_t> idx(v.size());
  iota(idx.begin(), idx.end(), 0);

  // sort indexes based on comparing values in v
  sort(idx.begin(), idx.end(),
       [&v](size_t i1, size_t i2) {return v[i1] > v[i2];});

  return idx;
}

// reference from https://github.com/scipy/scipy/blob/master/scipy/stats/kde.py
// and https://github.com/CGATOxford/UMI-tools/blob/master/umi_tools/umi_methods.py#L193
bool gaussianKDE(const std::vector<uint32_t>& freqCounter,
                 const std::vector<size_t>& sortedIdx,
                 double& invCovariance, double& normFactor,
                 uint32_t expect_cells, uint32_t& predicted_cells){
  double covariance{0.0}, mean{0.0}, bw_method {0.01}, threshold = 0.001*freqCounter[sortedIdx[0]];
  std::vector<double> logDataset;
  size_t numElem, xSpace {10000};

  // extract counts above threshold
  for(size_t i=0; i<freqCounter.size(); i++){
    double count = static_cast<double>(freqCounter[ sortedIdx[ i ] ]);
    if (count <= threshold){
      break;
    }
    count = std::log10(count);
    mean += count;
    logDataset.emplace_back(count);
  }

  // size of the reference
  numElem = logDataset.size();

  // get mean of the data
  mean /= numElem;

  // generate the covariance
  for (auto count: logDataset){
    covariance += std::pow((count - mean), 2);
  }
  covariance = (covariance * bw_method) / (numElem - 1.0);

  if (covariance == 0){
    std::cout << "0 Covariance error for Gaussian kde"<<std::flush;
    exit(1);
  }

  const double PI = 3.1415926535897;
  invCovariance = 1.0 / covariance;
  normFactor = std::sqrt( 2.0*PI*covariance ) * numElem;

  // Evaluate Step starting from here
  double decrement = (logDataset[0] - logDataset[logDataset.size()-1]) / static_cast<double>(xSpace);
  std::vector<double> density(xSpace, 0.0);

  for (size_t i=0; i<numElem; i++){
    double pred = logDataset[0];
    for(size_t j=0; j<xSpace; j++,pred -= decrement){
      double diff = logDataset[ i ] - pred;
      double energy = ( std::pow(diff,2)*invCovariance ) / 2.0;
      density[j] += std::exp(-energy);
    }
  }

  //calculating the argrelextrema
  std::vector<uint32_t> local_mins;
  for (size_t i=1; i<xSpace-1; i++){
    if (density[i-1] > density[i] and density[i] < density[i+1]){
      local_mins.emplace_back(i);
    }
  }

  for (auto minIdx: local_mins){
    double freqThreshold = std::pow(10, logDataset[0]-(minIdx*decrement));
    size_t boundary {0};
    while( freqThreshold <= freqCounter[sortedIdx[boundary]] ){
      boundary ++;
    }
    if (boundary > expect_cells){
      predicted_cells = boundary;
      return false;
    }
    else if (expect_cells*0.1 > boundary){
      continue;
    }
    else{
      predicted_cells = boundary;
      return true;
    }
  }
  predicted_cells = 0;
  return false;
}

uint32_t getLeftBoundary(std::vector<size_t>& sortedIdx,
                         uint32_t topxBarcodes,
                         const std::vector<uint32_t>& freqCounter){
  // iterate in reverse order since sortedIdx is sorted in decreasing
  // order
  double cumCount{0.0};
  std::vector<double> freqs(topxBarcodes);
  for(uint32_t i = 0; i < topxBarcodes; i++){
    size_t ind = sortedIdx[topxBarcodes-i-1];
    cumCount += freqCounter[ind];
    freqs[i] = std::log(cumCount);
  }

  std::vector<uint32_t> X(topxBarcodes);
  std::iota(X.begin(), X.end(), 0);

  bool isUp;
  uint32_t x;
  double y, slope, leftExtreme{freqs[0]};
  for(uint32_t j=0; j<topxBarcodes; j++){
    x = X[j];
    y = freqs[j];

    if(y == leftExtreme){
      continue;
    }

    size_t nextBcIdx(j+1);
    std::vector<double> Y(topxBarcodes-nextBcIdx);
    slope = y/x;
    // fill in the values for fitted line
    std::transform(X.begin()+nextBcIdx, X.end(), Y.begin(),
                   [slope](uint32_t i) {return i*slope;} );

    isUp = false;
    double curveY, lineY;
    for(auto i=nextBcIdx; i<topxBarcodes; i++){
      curveY = freqs[i];
      lineY = Y[i-nextBcIdx];
      if (lineY > curveY){
        isUp = true;
        break;
      }
    }

    if(isUp == false){
      // ignoring all the frequencies having same frequency as cutoff
      uint32_t cutoff = topxBarcodes-j;
      uint32_t cutoffFrequency = freqCounter[sortedIdx[cutoff]];
      uint32_t nearestLeftFrequency = cutoffFrequency;
      while(nearestLeftFrequency == cutoffFrequency){
        nearestLeftFrequency = freqCounter[sortedIdx[--cutoff]];
      }
      return cutoff;
    }
  }

  return 0;
}

/*
  Knee calculation and sample true set of barcodes
 */
template <typename ProtocolT>
void sampleTrueBarcodes(const std::vector<uint32_t>& freqCounter,
                        TrueBcsT& trueBarcodes, size_t& lowRegionNumBarcodes,
                        std::unordered_map<uint32_t, nonstd::basic_string_view<char>> colMap,
                        AlevinOpts<ProtocolT>& aopt){
  std::vector<size_t> sortedIdx = sort_indexes(freqCounter);
  size_t maxNumBarcodes { aopt.maxNumBarcodes }, lowRegionMaxNumBarcodes { 1000 };
  size_t lowRegionMinNumBarcodes { aopt.lowRegionMinNumBarcodes };
  double lowConfidenceFraction { 0.5 };
  uint32_t topxBarcodes = std::min(maxNumBarcodes, freqCounter.size());
  uint64_t history { 0 };
  uint32_t threshold;

  if (aopt.forceCells > 0) {
    topxBarcodes = aopt.forceCells;
  }
  else if (aopt.expectCells > 0){
    // Expect Cells algorithm is taken from
    // https://github.com/10XGenomics/cellranger/blob/e5396c6c444acec6af84caa7d3655dd33a162852/lib/python/cellranger/stats.py#L138

    constexpr uint32_t MAX_FILTERED_CELLS = 2;
    constexpr double UPPER_CELL_QUANTILE = 0.01;
    constexpr double FRACTION_MAX_FREQUENCY = 0.1;

    uint32_t baselineBcs = std::max(1, static_cast<int32_t>( aopt.expectCells*UPPER_CELL_QUANTILE ));
    double cutoffFrequency = std::max(1.0, freqCounter[sortedIdx[baselineBcs]] * FRACTION_MAX_FREQUENCY);
    uint32_t maxNumCells = std::min(static_cast<uint32_t>(freqCounter.size()), aopt.expectCells * MAX_FILTERED_CELLS);

    topxBarcodes = maxNumCells;
    for(size_t i=baselineBcs; i<maxNumCells; i++){
      if (freqCounter[sortedIdx[i]] < cutoffFrequency) {
        topxBarcodes = i;
        break;
      }
    }

    if (topxBarcodes == maxNumCells){
      aopt.jointLog->error("Can't find right Boundary.\n"
                           "Please Report this issue on github.");
      aopt.jointLog->flush();
      exit(1);
    }
  }
  else{
    topxBarcodes = getLeftBoundary(sortedIdx,
                                   topxBarcodes,
                                   freqCounter);
    if (topxBarcodes == 0){
      aopt.jointLog->error("Can't find left Boundary.\n"
                           "Please Report this issue on github.");
      aopt.jointLog->flush();
      exit(1);
    }
    else{
      aopt.jointLog->info("Knee found left boundary at {} {} {}",
                          green, topxBarcodes, RESET_COLOR);

      double invCovariance {0.0}, normFactor{0.0};
      uint32_t gaussThreshold;
      bool isGaussOk = gaussianKDE(freqCounter, sortedIdx,
                                   invCovariance, normFactor,
                                   topxBarcodes, gaussThreshold);

      if ( isGaussOk ){
        topxBarcodes = gaussThreshold;
        // consider only if within 10% of current prediction
        aopt.jointLog->info("Gauss Corrected Boundary at {} {} {}",
                            green, gaussThreshold, RESET_COLOR);
      }
      else{
        aopt.jointLog->warn("Gauss Prediction {} Too far from knee prediction skipping it",
                            gaussThreshold);
      }

      aopt.jointLog->info("Learned InvCov: {} normfactor: {}",
                          invCovariance, normFactor);
      if (invCovariance == 0.0 or normFactor == 0.0){
        aopt.jointLog->error("Wrong invCovariance/Normfactor");
        aopt.jointLog->flush();
        exit(1);
      }
    }
  }//end-left-knee finding case

  uint32_t fractionTrueBarcodes = static_cast<int>(lowConfidenceFraction * topxBarcodes);

  if (fractionTrueBarcodes < lowRegionMinNumBarcodes){
    lowRegionNumBarcodes = lowRegionMinNumBarcodes;
  }
  else if (fractionTrueBarcodes > lowRegionMaxNumBarcodes){
    lowRegionNumBarcodes = lowRegionMaxNumBarcodes;
  }
  else{
    lowRegionNumBarcodes = fractionTrueBarcodes;
  }

  // ignoring all the frequencies having same frequency as cutoff
  // to imitate stable sort
  topxBarcodes += lowRegionNumBarcodes;
  uint32_t cutoffFrequency = freqCounter[sortedIdx[ topxBarcodes ]];
  uint32_t nearestLeftFrequency = cutoffFrequency;
  while(nearestLeftFrequency == cutoffFrequency){
    nearestLeftFrequency = freqCounter[sortedIdx[--topxBarcodes]];
    lowRegionNumBarcodes--;
  }
  lowRegionNumBarcodes++;
  topxBarcodes++;

  uint64_t totalReads{0};
  for(size_t i=0; i<topxBarcodes; i++){
    totalReads += freqCounter[sortedIdx[i]];
  }

  uint64_t readsThrownCounter {0};
  for(size_t i=topxBarcodes; i<freqCounter.size(); i++){
    readsThrownCounter += freqCounter[sortedIdx[i]];
  }
  totalReads += readsThrownCounter;

  double percentThrown = readsThrownCounter*100.0 / totalReads;
  if (percentThrown > 50.0) {
    aopt.jointLog->warn("Total {}% reads will be thrown away"
                        " because of noisy Cellular barcodes.",
                        percentThrown);
  }
  else{
    aopt.jointLog->info("Total {}% reads will be thrown away"
                        " because of noisy Cellular barcodes.",
                        percentThrown);
  }

  // keeping some cells left of the left boundary for learning
  aopt.jointLog->info("Total {}{}{}(has {}{}{} low confidence)"
                      " barcodes",
                      green, topxBarcodes, RESET_COLOR,
                      green, lowRegionNumBarcodes, RESET_COLOR);

  threshold = topxBarcodes;

  if(aopt.dumpfeatures){
    auto frequencyFileName = aopt.outputDirectory / "raw_cb_frequency.txt";
    std::ofstream freqFile;
    freqFile.open(frequencyFileName.string());
    for (auto i:sortedIdx){
      uint32_t count = freqCounter[i];
      if (count == 0){
        break;
      }
      freqFile << colMap[i] << "\t"
               << count << "\n";
      topxBarcodes--;
    }
    freqFile.close();
  }

  for (size_t i=0; i<threshold; i++){
    trueBarcodes.insert(nonstd::to_string(colMap[sortedIdx[i]]));
  }
  aopt.numCells = trueBarcodes.size();
}

/*
  Index barcodes: map each possible 16M barcodes to one
  element of the set of true Barcode.
 */
template <typename ProtocolT>
void indexBarcodes(AlevinOpts<ProtocolT>& aopt,
                   CFreqMapT& freqCounter,
                   TrueBcsT& trueBarcodes,
                   SoftMapT& barcodeSoftMap){
  std::unordered_set<std::string> neighbors;
  std::unordered_map<std::string, std::vector<std::string>> ZMatrix;
  uint32_t wrngWhiteListCount{0};
  for (const auto trueBarcode: trueBarcodes){
    neighbors.clear();
    //find all neighbors to this true barcode
    aut::findNeighbors(aopt.protocol.barcodeLength,
                       trueBarcode,
                       neighbors);

    for(const auto& neighbor : neighbors){
      uint32_t freq{0};
      auto indexIt = freqCounter.find(neighbor);
      bool indexOk = indexIt != freqCounter.end();
      //bool indexOk = freqCounter.find(neighbor, freq);
      bool inTrueBc = trueBarcodes.find(neighbor) != trueBarcodes.end();
      if(not inTrueBc and indexOk and *indexIt > aopt.freqThreshold){
        ZMatrix[neighbor].push_back(trueBarcode);
      }
    }

    //bool inTrueBc = freqCounter.contains(trueBarcode);
    bool inTrueBc = freqCounter.find(trueBarcode) != freqCounter.end();
    if (not inTrueBc){
      wrngWhiteListCount += 1;
    }
  }//end-for
  //Done filling ZMatrix

  aopt.jointLog->info("Done populating Z matrix");
  if(trueBarcodes.size() - wrngWhiteListCount < 50){
    aopt.jointLog->warn("{} Whitelisted Barcodes with 0 frequency", wrngWhiteListCount);
  }

  std::string barcode;
  std::vector<std::pair<std::string, double>> dumpPair;

  for(auto& ZRow:ZMatrix){ //loop over every row of sparse matrix
    barcode = ZRow.first;
    //Avi -> have to work more on model
    dumpPair.clear();
    alevin::model::coinTossBarcodeModel(barcode,
                                        aopt,
                                        ZRow.second,
                                        freqCounter,
                                        dumpPair);
    for(auto& updateVal: dumpPair){
      barcodeSoftMap[barcode].emplace_back(updateVal);
    }
  }//end-for

  if(aopt.dumpBarcodeMap){
    auto dumpMapFile = aopt.outputDirectory / "barcodeSoftMaps.txt";
    std::ofstream mapFile;
    mapFile.open(dumpMapFile.string());

    for(const auto& softMapIt: barcodeSoftMap){
      auto trBcVec = softMapIt.second;
      mapFile << softMapIt.first << "\t" << trBcVec.size();
      for (auto trBc: trBcVec){
        mapFile << "\t" << trBc.first << "\t" << trBc.second;
      }
      mapFile << "\n";
    }
    mapFile.close();
  }

  if(aopt.dumpUmiToolsMap){
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    std::unordered_map<std::string, std::vector<std::string>> umitoolsMap;

    for(auto trBc: trueBarcodes){
      umitoolsMap[trBc] = std::vector<std::string>();
    }

    auto umitoolsMapFile = aopt.outputDirectory / "umitoolsMap.txt";
    std::ofstream utFile;
    utFile.open(umitoolsMapFile.string());
    for(const auto& softMapIt: barcodeSoftMap){
      std::string trBc, bc{softMapIt.first};
      auto trBcVec = softMapIt.second;

      if(trBcVec.size() == 1){
        trBc = trBcVec.front().first;
      }
      else{
        double rn = dist(mt);
        for(auto dp: trBcVec){
          if(rn < dp.second){
            trBc = dp.first;
            break;
          }
        }
      }

      umitoolsMap[trBc].push_back(bc);
    }

    for (auto elem: umitoolsMap){
      auto trBc = elem.first;
      utFile << trBc << "\t";
      for (auto bc : elem.second){
        utFile << bc << ",";
      }
      utFile << "\b\n";
    }
    utFile.close();
  }
}

template <typename ProtocolT>
bool writeFastq(AlevinOpts<ProtocolT>& aopt,
                paired_parser_qual* parser,
                SoftMapT& barcodeMap,
                std::mutex& ioMutex,
                TrueBcsT& trueBarcodes){
  size_t rangeSize{0};
  uint32_t totNumBarcodes{0};
  std::string barcode;
  std::string umi;

  std::random_device rd;
  std::mt19937 mt(rd());
  std::uniform_real_distribution<double> dist(0.0, 1.0);

  try{
    auto rg = parser->getReadGroup();
    auto log = aopt.jointLog;

    fmt::print(stderr, "\n\n");
    while (parser->refill(rg)) {
      rangeSize = rg.size();
      for (size_t i = 0; i < rangeSize; ++i) { // For all the read in this batch
        auto& rp = rg[i];
        if(aopt.protocol.end == bcEnd::FIVE){
          {
            nonstd::optional<std::string> extractedSeq = aut::extractBarcode(rp.first.seq, aopt.protocol);
            bool seqOk = (extractedSeq.has_value()) ? aut::sequenceCheck(*extractedSeq) : false;
            if (not seqOk){
              continue;
              //std::cerr<<"Can't extract barcode";
              //return false;
            }
            barcode = *extractedSeq;
          }

          {
            bool seqOk = aut::extractUMI(rp.first.seq, aopt.protocol, umi);
            if (not seqOk){
              std::cerr<<"Can't extract UMI";
              return false;
            }
          }
        }
        //else if (aopt.protocol.end == bcEnd::THREE) {
        //  std::string seq = rp.first.seq;
        //  std::reverse(seq.begin(), seq.end());
        //  barcode = rp.first.seq.substr(0, aopt.protocol.barcodeLength);
        //  umi = rp.first.seq.substr(aopt.protocol.barcodeLength,
        //                            aopt.protocol.umiLength);
        //}

        bool inTrueBc = trueBarcodes.find(barcode) != trueBarcodes.end();
        auto it = barcodeMap.find(barcode);
        bool inBarMap = it != barcodeMap.end();
        std::string corrBarcode;

        if(inTrueBc){
          corrBarcode = barcode;
        }
        else if(inBarMap and it->second.size() > 1){
          //toss a [0,1) real valued coin
          double rn = dist(mt);

          for (auto bcPair: it->second){
            if (rn < bcPair.second){
              corrBarcode = bcPair.first;
              break;
            }
          }
        }
        else if(inBarMap){
          corrBarcode = it->second.front().first;
        }
        else{
          continue;
        }

        std::cout << "@" << rp.second.name << "_" << corrBarcode << "_" << umi << "\n"
                  << rp.second.seq << "\n"
                  << "+" << "\n"
                  << rp.second.qual << "\n";

        totNumBarcodes += 1;
        if (totNumBarcodes % 500000 == 0) {
          char red[] = "\x1b[30m";
          red[3] = '0' + static_cast<char>(fmt::RED);
          fmt::print(stderr, "\r\r{}Dumped{} {} {}reads{}", green, red,
                     totNumBarcodes, green, RESET_COLOR);
        }
      }//end-for
    }//end-while
    fmt::print(stderr, "\n");
    return true;
  }
  catch (std::exception& e) {
    std::cerr << "Exception : [" << e.what() << "]\n" << std::flush;
    return false;
  }
}

/*
  function to Rapidly parse through the barcode file, generate density
  of each Unique barcode, use knee method to select true barcodes and
  use our model to generate mapping of each 16M barcodes to true/null
  barcode.
 */
template <typename ProtocolT>
void processBarcodes(std::vector<std::string>& barcodeFiles,
                     std::vector<std::string>& readFiles,
                     AlevinOpts<ProtocolT>& aopt,
                     SoftMapT& barcodeSoftMap,
                     TrueBcsT& trueBarcodes,
                     CFreqMapT& freqCounter,
                     size_t& numLowConfidentBarcode){
  if (not aopt.nobarcode){
    // Barcode processing always uses 2 threads now,
    // one parsing thread and one consumer thread.
    std::unique_ptr<single_parser> singleParserPtr{nullptr};
    constexpr uint32_t miniBatchSize{5000};
    uint32_t numParsingThreads{aopt.numParsingThreads},
      numThreads{aopt.numConsumerThreads};
    std::vector<std::thread> threads;
    std::mutex ioMutex;
    std::atomic<uint64_t> totNumBarcodes{0}, usedNumBarcodes{0};

    if (aopt.numThreads <= 3) {
      numThreads = 1;
    }

    //Populating Barcode Density Vector
    singleParserPtr.reset(new single_parser(barcodeFiles, numThreads,
                                            numParsingThreads, miniBatchSize));

    singleParserPtr->start();
    densityCalculator(singleParserPtr.get(), aopt,
                      freqCounter, usedNumBarcodes, totNumBarcodes);
    singleParserPtr->stop();

    fmt::print(stderr, "\n\n");
    aopt.jointLog->info("Done barcode density calculation.");
    aopt.jointLog->info("# Barcodes Used: {}{}{} / {}{}{}.",
                        green, usedNumBarcodes, RESET_COLOR,
                        red,totNumBarcodes, RESET_COLOR);

    //import whitelist barcodes if present
    if(boost::filesystem::exists(aopt.whitelistFile)){
      std::ifstream whiteFile(aopt.whitelistFile.string());
      std::string whtBc;
      if(whiteFile.is_open()) {
        while(getline(whiteFile, whtBc)) {
          trueBarcodes.insert(whtBc);
        }
        whiteFile.close();
      }
      aopt.jointLog->info("Done importing white-list Barcodes");
      if (aopt.debug) {
        std::vector<std::string> skippedTrueBarcodes ;
        for ( auto trueBarcode: trueBarcodes ) {
          auto it = freqCounter.find(trueBarcode);
          if (it == freqCounter.end() ) {
            skippedTrueBarcodes.emplace_back(trueBarcode);
          }
        }

        if ( skippedTrueBarcodes.size() > 0 ) {
          aopt.jointLog->warn("Skipping {} Barcodes with 0 reads"
                              "\n Assuming this is the required behavior.",
                              skippedTrueBarcodes.size());
          for (auto trueBarcode: skippedTrueBarcodes) {
            trueBarcodes.erase(trueBarcode);
          }
        }
      }

      aopt.jointLog->info("Total {} white-listed Barcodes", trueBarcodes.size());
    }
    else {
      std::vector<uint32_t> collapsedfrequency;
      std::unordered_map<uint32_t, nonstd::basic_string_view<char>> collapMap;
      size_t ind{0};
      for(auto fqIt = freqCounter.begin(); fqIt != freqCounter.end(); ++fqIt){
        collapsedfrequency.push_back(fqIt.value());
        collapMap[ind] = fqIt.key_sv();
        ind += 1;
      }

      //Calculate the knee using the frequency distribution
      //and get the true set of barcodes
      sampleTrueBarcodes(collapsedfrequency, trueBarcodes,
                           numLowConfidentBarcode, collapMap, aopt);

      aopt.jointLog->info("Done True Barcode Sampling");
    }

    indexBarcodes(aopt, freqCounter, trueBarcodes, barcodeSoftMap);
    aopt.jointLog->info("Done indexing Barcodes");

    aopt.jointLog->info("Total Unique barcodes found: {}", freqCounter.size());
    aopt.jointLog->info("Used Barcodes except Whitelist: {}", barcodeSoftMap.size());

    uint32_t mmBcCounts{0}, mmBcReadCount{0};
    std::unordered_set<std::string> softMapWhiteBcSet;
    for(auto& softMapIt: barcodeSoftMap ){
      auto indexIt = freqCounter.find(softMapIt.first);
      bool indexOk = indexIt != freqCounter.end();
      if ( not indexOk){
        aopt.jointLog->error("Error: index not find in freq Counter\n"
                             "Please Report the issue on github");
        aopt.jointLog->flush();
        exit(1);
      }

      uint32_t numReads;
      numReads = *indexIt;

      std::vector<std::pair<std::string, double>>& trBcVec = softMapIt.second;
      if (not aopt.noSoftMap and trBcVec.size() > 1){
        mmBcCounts += 1;

        for(std::pair<std::string, double> whtBc : trBcVec){
          softMapWhiteBcSet.insert(whtBc.first);
        }
        mmBcReadCount += numReads;
      }

      // NOTE: Have to update the ambiguity resolution
      if (aopt.noSoftMap){
        while(trBcVec.size() != 1){
          trBcVec.pop_back();
        }
        trBcVec.front().second = 1.0;
      }

      std::string trBc = trBcVec.front().first;
      freqCounter[trBc] += numReads;
      freqCounter.erase(softMapIt.first);
    }

    if(aopt.dumpfeatures){
      auto frequencyFileName = aopt.outputDirectory / "filtered_cb_frequency.txt";
      std::ofstream freqFile;
      freqFile.open(frequencyFileName.string());
      for(auto it = freqCounter.begin(); it != freqCounter.end(); ++it) {
        auto trBc = it.key();
        auto trIt = trueBarcodes.find(trBc);
        if ( trIt != trueBarcodes.end() ) {
          freqFile << trBc << "\t"
                   << it.value() << "\n";
        }
      }
      freqFile.close();
    }

    if (not aopt.noSoftMap){
      aopt.jointLog->info("Total Ambiguous Barcodes(soft-assigned):  {}", mmBcCounts);
      aopt.jointLog->info("Total CB-level Soft-Assignable Reads:  {}", mmBcReadCount);
      aopt.jointLog->info("Total whitelist-cells ambiguous reads can be assigned to: {}",
                          softMapWhiteBcSet.size());
      aopt.jointLog->info("Expected gain/cell using Alevin: {}", mmBcReadCount/softMapWhiteBcSet.size());
    }

    if (aopt.dumpfq){
      std::unique_ptr<paired_parser_qual> pairedParserQualPtr{nullptr};
      pairedParserQualPtr.reset(new paired_parser_qual(barcodeFiles, readFiles,
                                                       1, 1, miniBatchSize));
      pairedParserQualPtr->start();
      bool isDumpok = writeFastq(aopt, pairedParserQualPtr.get(),
                                 barcodeSoftMap, ioMutex, trueBarcodes);
      pairedParserQualPtr->stop();
      if(!isDumpok){
        aopt.jointLog->error("Not able to dump fastq."
                             "Something went wrong.\n"
                             "Please report this issue to github");
        aopt.jointLog->flush();
        std::exit(1);
      }
      aopt.jointLog->info("Done dumping fastq File");
    }
  } else{
    trueBarcodes.insert("AAA");
  }
}

template <typename ProtocolT, typename OrderedOptionsT>
void initiatePipeline(AlevinOpts<ProtocolT>& aopt,
                      SalmonOpts& sopt,
                      OrderedOptionsT& orderedOptions,
                      boost::program_options::variables_map& vm,
                      std::string commentString,
                      std::vector<std::string> barcodeFiles,
                      std::vector<std::string> readFiles){
  bool isOptionsOk = aut::processAlevinOpts(aopt, sopt, vm);
  if (!isOptionsOk){
    exit(1);
  }

  // If we're supposed to be quiet, set the global logger level to >= warn
  if (aopt.quiet) {
    spdlog::set_level(spdlog::level::warn); //Set global log level to warn
  }
  else {
    fmt::print(stderr, "{}\n\n", commentString);
  }

  /*
    Barcode Knee generation
  */
  SoftMapT barcodeSoftMap;
  TrueBcsT trueBarcodes;
  //frequency counter
  CFreqMapT freqCounter;
  //freqCounter.set_max_resize_threads(sopt.maxHashResizeThreads);
  freqCounter.reserve(2097152);

  size_t numLowConfidentBarcode;

  aopt.jointLog->info("Processing barcodes files (if Present) \n\n ");

  processBarcodes(barcodeFiles,
                  readFiles,
                  aopt,
                  barcodeSoftMap,
                  trueBarcodes,
                  freqCounter,
                  numLowConfidentBarcode);

  aopt.jointLog->flush();

  if(!aopt.noQuant){
    aopt.jointLog->info("Done with Barcode Processing; Moving to Quantify\n");
    alevinQuant(aopt, sopt, barcodeSoftMap, trueBarcodes,
                orderedOptions, freqCounter, numLowConfidentBarcode);
  }
  else{
    boost::filesystem::path cmdInfoPath = vm["output"].as<std::string>();
    // Write out information about the command / run
    bool isWriteOk = aut::writeCmdInfo(cmdInfoPath / "cmd_info.json", orderedOptions);
    if(!isWriteOk){
      fmt::print(stderr, "writing in output directory failed\n Exiting Now");
      exit(1);
    }
  }
}

int salmonBarcoding(int argc, char* argv[]) {
  namespace bfs = boost::filesystem;
  namespace po = boost::program_options;

  std::vector<std::string> barcodeFiles, readFiles, unmateFiles;
  bool optChain{false};
  int32_t numBiasSamples{0};

  double coverageThresh;
  //vector<string> unmatedReadFiles;
  //vector<string> mate1ReadFiles;
  //vector<string> mate2ReadFiles;

  SalmonOpts sopt;
  auto tot_cores = std::thread::hardware_concurrency();
  sopt.numThreads = std::max(1, static_cast<int>(tot_cores/4.0));

  salmon::ProgramOptionsGenerator pogen;

  auto inputOpt = pogen.getMappingInputOptions(sopt);
  auto mapSpecOpt = pogen.getMappingSpecificOptions(sopt);
  auto advancedOpt = pogen.getAdvancedOptions(numBiasSamples, sopt);
  auto hiddenOpt = pogen.getHiddenOptions(sopt);
  auto testingOpt = pogen.getTestingOptions(sopt);
  auto deprecatedOpt = pogen.getDeprecatedOptions(sopt);
  auto alevinBasicOpt = pogen.getAlevinBasicOptions(sopt);
  auto alevinDevsOpt = pogen.getAlevinDevsOptions();

  po::options_description all("alevin options");
  all.add(inputOpt).add(alevinBasicOpt).add(alevinDevsOpt).add(mapSpecOpt).add(advancedOpt).add(testingOpt).add(hiddenOpt).add(deprecatedOpt);

  po::options_description visible("alevin options");
  visible.add(inputOpt).add(alevinBasicOpt);

  po::variables_map vm;
  try {
    auto orderedOptions =
        po::command_line_parser(argc, argv).options(all).run();

    po::store(orderedOptions, vm);

    if (vm.count("help")) {
      auto hstring = R"(
alevin
==========
salmon-based processing of single-cell RNA-seq data.
)";

      std::cout << hstring << std::endl;
      std::cout << visible << std::endl;
      std::exit(0);
    }

    po::notify(vm);

    green[3] = '0' + static_cast<char>(fmt::GREEN);
    red[3] = '0' + static_cast<char>(fmt::RED);


    bool dropseq = vm["dropseq"].as<bool>();
    bool indrop = vm["indrop"].as<bool>();
    bool chrom = vm["chromium"].as<bool>();
    bool gemcode = vm["gemcode"].as<bool>();
    bool celseq = vm["celseq"].as<bool>();

    uint8_t validate_num_protocols {0};
    if (dropseq) validate_num_protocols += 1;
    if (indrop) validate_num_protocols += 1;
    if (chrom) validate_num_protocols += 1;
    if (gemcode) validate_num_protocols += 1;
    if (celseq) validate_num_protocols += 1;

    if ( validate_num_protocols != 1 ) {
      fmt::print(stderr, "ERROR: Please specify one and only one scRNA protocol;");
      exit(1);
    }

    std::stringstream commentStream;
    commentStream << "### alevin (dscRNA-seq quantification) v" << salmon::version << "\n";
    commentStream << "### [ program ] => salmon \n";
    commentStream << "### [ command ] => alevin \n";
    for (auto& opt : orderedOptions.options) {
      commentStream << "### [ " << opt.string_key << " ] => {";
      for (auto& val : opt.value) {
        commentStream << " " << val;
      }
      commentStream << " }\n";
    }
    std::string commentString = commentStream.str();

    // Until we can figure out a better way to generify our parsing
    barcodeFiles = sopt.mate1ReadFiles;
    readFiles = sopt.mate2ReadFiles;
    unmateFiles = sopt.unmatedReadFiles;

    if (dropseq){
      AlevinOpts<apt::DropSeq> aopt;
      //aopt.jointLog->warn("Using DropSeq Setting for Alevin");
      initiatePipeline(aopt, sopt, orderedOptions,
                       vm, commentString,
                       barcodeFiles, readFiles);
    }
    else if(indrop){
      std::cout<<"Indrop get neighbors removed, please use other protocols";
      exit(1);
      if(vm.count("w1") != 0){
        std::string w1 = vm["w1"].as<std::string>();
        AlevinOpts<apt::InDrop> aopt;
        aopt.protocol.setW1(w1);
        //aopt.jointLog->warn("Using InDrop Setting for Alevin");
        initiatePipeline(aopt, sopt, orderedOptions,
                         vm, commentString,
                         barcodeFiles, readFiles);
      }
      else{
        fmt::print(stderr, "ERROR: indrop needs w1 flag too.\n Exiting Now");
        exit(1);
      }
    }
    else if(chrom){
      AlevinOpts<apt::Chromium> aopt;
      //aopt.jointLog->warn("Using 10x v2 Setting for Alevin");
      initiatePipeline(aopt, sopt, orderedOptions,
                       vm, commentString,
                       barcodeFiles, readFiles);
    }
    else if(gemcode){
      AlevinOpts<apt::Gemcode> aopt;
      //aopt.jointLog->warn("Using 10x v1 Setting for Alevin");
      initiatePipeline(aopt, sopt, orderedOptions,
                       vm, commentString,
                       unmateFiles, readFiles);
    }
    else if(celseq){
      AlevinOpts<apt::CELSeq> aopt;
      //aopt.jointLog->warn("Using CEL-Seq Setting for Alevin");
      initiatePipeline(aopt, sopt, orderedOptions,
                       vm, commentString,
                       barcodeFiles, readFiles);
    }
    else{
      AlevinOpts<apt::Custom> aopt;
      //aopt.jointLog->warn("Using Custom Setting for Alevin");
      initiatePipeline(aopt, sopt, orderedOptions,
                       vm, commentString,
                       barcodeFiles, readFiles);
    }

  } catch (po::error& e) {
    std::cerr << "Exception : [" << e.what() << "]. Exiting.\n";
    std::exit(1);
  } catch (const spdlog::spdlog_ex& ex) {
    std::cerr << "logger failed with : [" << ex.what() << "]. Exiting.\n";
    std::exit(1);
  } catch (std::exception& e) {
    std::cerr << "Exception : [" << e.what() << "]\n";
    std::cerr << argv[0] << " alevin was invoked improperly.\n";
    std::cerr << "For usage information, try " << argv[0]
              << " alevin --help\nExiting.\n";
    std::exit(1);
  }

  return 0;
}
