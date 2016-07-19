/* ---------------------------------------------------------------------
 * Numenta Platform for Intelligent Computing (NuPIC)
 * Copyright (C) 2016, Numenta, Inc.  Unless you have an agreement
 * with Numenta, Inc., for a separate license for this software code, the
 * following terms and conditions apply:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Affero Public License for more details.
 *
 * You should have received a copy of the GNU Affero Public License
 * along with this program.  If not, see http://www.gnu.org/licenses.
 *
 * http://numenta.org/licenses/
 * ---------------------------------------------------------------------
 */

#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <sstream>
#include <vector>
#include <stdio.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/std/iostream.h>

#include <nupic/algorithms/ClassifierResult.hpp>
#include <nupic/algorithms/SDRClassifier.hpp>
#include <nupic/proto/SdrClassifier.capnp.h>
#include <nupic/math/ArrayAlgo.hpp>
#include <nupic/utils/Log.hpp>

using namespace std;

namespace nupic
{
	namespace algorithms 
	{
	  namespace sdr_classifier
	  {

	    SDRClassifier::SDRClassifier(
	        const vector<UInt>& steps, Real64 alpha, Real64 actValueAlpha,
	        UInt verbosity) : alpha_(alpha), actValueAlpha_(actValueAlpha),
	        learnIteration_(0), recordNumMinusLearnIteration_(0), 
	        maxInputIdx_(0), maxBucketIdx_(0), version_(Version), 
	        verbosity_(verbosity)
	    {
	      for (const auto& step : steps)
	      {
	        steps_.push_back(step);
	      }
	      recordNumMinusLearnIterationSet_ = false;
	      maxSteps_ = 0;
	      for (auto& elem : steps_)
	      {
	        UInt current = elem + 1;
	        if (current > maxSteps_)
	        {
	          maxSteps_ = current;
	        }
	      }
	      actualValues_.push_back(0.0);
	      actualValuesSet_.push_back(false);

	      // TODO: insert maxBucketIdx / maxInputIdx hint as parameter?
	      // There can be great overhead reallocating the array every time a new
	      // input is seen, especially if we start at (0, 0). The client will
	      // usually know what is the final maxInputIdx (typically the number
	      // of columns?), and we can have heuristics using the encoder's 
	      // settings to get an good approximate of the maxBucketIdx, thus having
	      // to reallocate this matrix only a few times, even never if we use
	      // lower bounds
	      for (const auto& step : steps_)
	      {
	      	Matrix weights = Matrix(maxInputIdx_ + 1, maxBucketIdx_ + 1);
	      	weightMatrix_.insert(pair<UInt, Matrix>(step, weights));
	      }
	    }

	    SDRClassifier::~SDRClassifier()
	    {
	    }

	    void SDRClassifier::compute(
	      UInt recordNum, const vector<UInt>& patternNZ, UInt bucketIdx,
	      Real64 actValue, bool category, bool learn, bool infer,
	      ClassifierResult* result)
	    {
	    	// save the offset between recordNum and learnIteration_ if this
	    	// was not set (first call to compute)
	    	if (!recordNumMinusLearnIterationSet_)
	    	{
	    		recordNumMinusLearnIteration_ = recordNum - learnIteration_;
	    		recordNumMinusLearnIterationSet_ = true;
	    	}

	    	// update learnIteration_
	    	learnIteration_ = recordNum - recordNumMinusLearnIteration_;

	    	// update pattern history
        patternNZHistory_.emplace_front(patternNZ.begin(), patternNZ.end());
        iterationNumHistory_.push_front(learnIteration_);
        if (patternNZHistory_.size() > maxSteps_)
        {
          patternNZHistory_.pop_back();
          iterationNumHistory_.pop_back();
        }

        // if input pattern has greater index than previously seen, update 
        // maxInputIdx and augment weight matrix with zero padding
        UInt maxInputIdx = *max_element(patternNZ.begin(), patternNZ.end());
        if (maxInputIdx > maxBucketIdx_)
        {
        	maxInputIdx_ = maxInputIdx;
        	for (const auto& step : steps_)
        	{	
	        	weightMatrix_[step].resize(maxInputIdx_ + 1, maxBucketIdx_ + 1);
          }
        }

        // if in inference mode, compute likelihood and update return value
        if (infer)
        {
        		infer_(patternNZ, bucketIdx, actValue, result);
        }

        // update weights if in learning mode
        if (learn)
        {
        	// if bucket is greater, updated maxBucketIdx_ and augment weight
        	// matrix with zero-padding
        	if (bucketIdx > maxBucketIdx_) 
        	{
        		maxBucketIdx_ = bucketIdx;
        		for (const auto& step : steps_)
        		{
		        	weightMatrix_[step].resize(maxInputIdx_ + 1, maxBucketIdx_ + 1);
        		}
        	}

        	// update rolling averages of bucket values
        	while (actualValues_.size() <= maxBucketIdx_)
          {
            actualValues_.push_back(0.0);
            actualValuesSet_.push_back(false);
          }
          if (!actualValuesSet_[bucketIdx] || category)
          {
            actualValues_[bucketIdx] = actValue;
            actualValuesSet_[bucketIdx] = true;
          } else {
            actualValues_[bucketIdx] =
                ((1.0 - actValueAlpha_) * actualValues_[bucketIdx]) +
                (actValueAlpha_ * actValue);
          }

          // compute errors and update weights
          deque<vector<UInt>>::const_iterator patternIteration =
              patternNZHistory_.begin();
          for (deque<UInt>::const_iterator learnIteration =
               iterationNumHistory_.begin();
               learnIteration !=iterationNumHistory_.end();
               learnIteration++, patternIteration++)
          {
            const vector<UInt> learnPatternNZ = *patternIteration;
            UInt nSteps = learnIteration_ - *learnIteration;

            // update weights
            if (binary_search(steps_.begin(), steps_.end(), nSteps))
            {
            	vector<Real64> error = calculateError_(bucketIdx, 
            		patternNZ, nSteps);
              for (auto& bit : learnPatternNZ)
              {
              	for (UInt bucket = 0; bucket < maxBucketIdx_; ++bucket)
              	{
              		weightMatrix_[nSteps].at(bit, 
              			bucket) += alpha_ * error[bucket];
              	}
              }
            }
          }

        }

	    }

      UInt SDRClassifier::persistentSize() const
      {
        stringstream s;
        s.flags(ios::scientific);
        s.precision(numeric_limits<double>::digits10 + 1);
        save(s);
        return s.str().size();
      }

	    void SDRClassifier::infer_(const vector<UInt>& patternNZ, UInt bucketIdx, 
	    	Real64 actValue, ClassifierResult* result)
	    {
    	  // add the actual values to the return value. For buckets that haven't
        // been seen yet, the actual value doesn't matter since it will have
        // zero likelihood.
        vector<Real64>* actValueVector = result->createVector(
            -1, actualValues_.size(), 0.0);
        for (UInt i = 0; i < actualValues_.size(); ++i)
        {
          if (actualValuesSet_[i])
          {
            (*actValueVector)[i] = actualValues_[i];
          } else {
            // if doing 0-step ahead prediction, we shouldn't use any
            // knowledge of the classification input during inference
            if (steps_.at(0) == 0)
            {
              (*actValueVector)[i] = 0;
            } else {
              (*actValueVector)[i] = actValue;
            }
          }
        }

        for (auto nSteps = steps_.begin(); nSteps!=steps_.end(); ++nSteps)
        {
        	vector<Real64>* likelihoods = result->createVector(*nSteps, 
        		maxBucketIdx_ + 1, 1.0 / actualValues_.size());
        	for (auto& bit : patternNZ)
        	{
        		Matrix weights = weightMatrix_[*nSteps];
        		add(likelihoods->begin(), likelihoods->end(), weights.begin(bit),
        			weights.begin(bit + 1));
        	}
        	// compute softmax of raw scores
        	// TODO: fix potential overflow problem by shifting scores by their
        	// maximal value across buckets
      		range_exp(1.0, *likelihoods);
        	normalize(*likelihoods, 1.0, 1.0);
        }
	    }

	    vector<Real64> SDRClassifier::calculateError_(UInt bucketIdx, 
      	const vector<UInt> patternNZ, UInt step)
      {
      	// compute predicted likelihoods
    	  vector<Real64> likelihoods (maxBucketIdx_ + 1, 
    	  	1.0 / actualValues_.size());

      	for (auto& bit : patternNZ)
      	{
      		Matrix weights = weightMatrix_[step];
      		add(likelihoods.begin(), likelihoods.end(), weights.begin(bit),
      			weights.begin(bit + 1));
      	}
    		range_exp(1.0, likelihoods);
      	normalize(likelihoods, 1.0, 1.0);

      	// compute target likelihoods
      	vector<Real64> targetDistribution (maxBucketIdx_ + 1, 0.0);
      	targetDistribution[bucketIdx] = 1.0;

      	axby(-1.0, likelihoods, 1.0, targetDistribution);
      	return likelihoods;
      }

      void SDRClassifier::save(ostream& outStream) const
      {
        // Write a starting marker and version.
        outStream << "SDRClassifier" << endl;
        outStream << version_ << endl;

        // Store the simple variables first.
        outStream << version() << " "
                  << alpha_ << " "
                  << actValueAlpha_ << " "
                  << learnIteration_ << " "
                  << maxSteps_ << " "
                  << maxBucketIdx_ << " "
                  << maxInputIdx_ << " "
                  << verbosity_ << " "
                  << endl;

        // V1 additions.
        outStream << recordNumMinusLearnIteration_ << " "
                  << recordNumMinusLearnIterationSet_ << " ";
        outStream << iterationNumHistory_.size() << " ";
        for (const auto& elem : iterationNumHistory_)
        {
          outStream << elem << " ";
        }
        outStream << endl;

        // Store the different prediction steps.
        outStream << steps_.size() << " ";
        for (auto& elem : steps_)
        {
          outStream << elem << " ";
        }
        outStream << endl;

        // Store the pattern history.
        outStream << patternNZHistory_.size() << " ";
        for (auto& pattern : patternNZHistory_)
        {
          outStream << pattern.size() << " ";
          for (auto& pattern_j : pattern)
          {
            outStream << pattern_j << " ";
          }
        }
        outStream << endl;

        // Store weight matrix
        outStream << weightMatrix_.size() << " ";
        for (const auto& elem : weightMatrix_)
        {
          outStream << elem.first << " ";
          outStream << elem.second;
        }
        outStream << endl;

        // Store the actual values for each bucket.
        outStream << actualValues_.size() << " ";
        for (UInt i = 0; i < actualValues_.size(); ++i)
        {
          outStream << actualValues_[i] << " ";
          outStream << actualValuesSet_[i] << " ";
        }
        outStream << endl;

        // Write an ending marker.
        outStream << "~SDRClassifier" << endl;
      }


      void SDRClassifier::load(istream& inStream)
      {
        // Clean up the existing data structures before loading
        steps_.clear();
        iterationNumHistory_.clear();
        patternNZHistory_.clear();
        actualValues_.clear();
        actualValuesSet_.clear();
        weightMatrix_.clear();

        // Check the starting marker.
        string marker;
        inStream >> marker;
        NTA_CHECK(marker == "SDRClassifier");

        // Check the version.
        UInt version;
        inStream >> version;
        NTA_CHECK(version <= 1);

        // Load the simple variables.
        inStream >> version_
                 >> alpha_
                 >> actValueAlpha_
                 >> learnIteration_
                 >> maxSteps_
                 >> maxBucketIdx_
                 >> maxInputIdx_
                 >> verbosity_;

        UInt numIterationHistory;
        UInt curIterationNum;
        if (version == 1)
        {
          inStream >> recordNumMinusLearnIteration_
                   >> recordNumMinusLearnIterationSet_;
          inStream >> numIterationHistory;
          for (UInt i = 0; i < numIterationHistory; ++i)
          {
            inStream >> curIterationNum;
            iterationNumHistory_.push_back(curIterationNum);
          }
        } else {
          recordNumMinusLearnIterationSet_ = false;
        }

        // Load the prediction steps.
        UInt size;
        UInt step;
        inStream >> size;
        for (UInt i = 0; i < size; ++i)
        {
          inStream >> step;
          steps_.push_back(step);
        }

        // Load the input pattern history.
        inStream >> size;
        UInt vSize;
        for (UInt i = 0; i < size; ++i)
        {
          inStream >> vSize;
          patternNZHistory_.emplace_back(vSize);
          for (UInt j = 0; j < vSize; ++j)
          {
            inStream >> patternNZHistory_[i][j];
          }
          if (version == 0)
          {
            iterationNumHistory_.push_back(
                learnIteration_ - (size - i));
          }
        }

        // Load weight matrix.
        UInt numSteps;
        inStream >> numSteps;
        for (UInt s = 0; s < numSteps; ++s)
        {
          inStream >> step;
          // Insert the step to initialize the weight matrix
          weightMatrix_[step] = Matrix(maxInputIdx_ + 1, maxBucketIdx_ + 1);
          for (UInt i = 0; i <= maxInputIdx_; ++i)
          {
          	for (UInt j = 0; j <= maxBucketIdx_; ++j)
          	{
          		inStream >> weightMatrix_[step].at(i, j);
          	}
          }
        }

        // Load the actual values for each bucket.
        UInt numBuckets;
        Real64 actualValue;
        bool actualValueSet;
        inStream >> numBuckets;
        for (UInt i = 0; i < numBuckets; ++i)
        {
          inStream >> actualValue;
          actualValues_.push_back(actualValue);
          inStream >> actualValueSet;
          actualValuesSet_.push_back(actualValueSet);
        }

        // Check for the end marker.
        inStream >> marker;
        NTA_CHECK(marker == "~SDRClassifier");

        // Update the version number.
        version_ = Version;
      }

			void SDRClassifier::write(SdrClassifierProto::Builder& proto) const 
			{
        auto stepsProto = proto.initSteps(steps_.size());
        for (UInt i = 0; i < steps_.size(); i++)
        {
          stepsProto.set(i, steps_[i]);
        }

        proto.setAlpha(alpha_);
        proto.setActValueAlpha(actValueAlpha_);
        proto.setLearnIteration(learnIteration_);
        proto.setRecordNumMinusLearnIteration(recordNumMinusLearnIteration_);
        proto.setRecordNumMinusLearnIterationSet(
            recordNumMinusLearnIterationSet_);
        proto.setMaxSteps(maxSteps_);

        auto patternNZHistoryProto =
          proto.initPatternNZHistory(patternNZHistory_.size());
        for (UInt i = 0; i < patternNZHistory_.size(); i++)
        {
          const auto& pattern = patternNZHistory_[i];
          auto patternProto = patternNZHistoryProto.init(i, pattern.size());
          for (UInt j = 0; j < pattern.size(); j++)
          {
            patternProto.set(j, pattern[j]);
          }
        }

        auto iterationNumHistoryProto =
          proto.initIterationNumHistory(iterationNumHistory_.size());
        for (UInt i = 0; i < iterationNumHistory_.size(); i++)
        {
          iterationNumHistoryProto.set(i, iterationNumHistory_[i]);
        }

        proto.setMaxBucketIdx(maxBucketIdx_);
        proto.setMaxInputIdx(maxInputIdx_);

        auto weightMatrixProtos =
          proto.initWeightMatrix(weightMatrix_.size());
        UInt k = 0;
        for (const auto& stepWeightMatrix : weightMatrix_)
        {
          auto stepWeightMatrixProto = weightMatrixProtos[k];
          stepWeightMatrixProto.setSteps(stepWeightMatrix.first);
          auto weightProto = stepWeightMatrixProto.initWeight(
          	(maxInputIdx_ + 1) * (maxBucketIdx_ + 1)
          	);
          // flatten weight matrix, serialized as a list of floats
          UInt idx = 0;
          for (UInt i = 0; i <= maxInputIdx_; ++i)
          {
          	for (UInt j = 0; j <= maxBucketIdx_; ++j)
          	{
							weightProto.set(k, stepWeightMatrix.second.at(i, j));
							idx++;
          	}
          }
          k++;
        }

        auto actualValuesProto = proto.initActualValues(actualValues_.size());
        for (UInt i = 0; i < actualValues_.size(); i++)
        {
          actualValuesProto.set(i, actualValues_[i]);
        }

        auto actualValuesSetProto =
          proto.initActualValuesSet(actualValuesSet_.size());
        for (UInt i = 0; i < actualValuesSet_.size(); i++)
        {
          actualValuesSetProto.set(i, actualValuesSet_[i]);
        }

        proto.setVersion(version_);
        proto.setVerbosity(verbosity_);
      }

      void SDRClassifier::read(SdrClassifierProto::Reader& proto)
      {
        // Clean up the existing data structures before loading
        steps_.clear();
        iterationNumHistory_.clear();
        patternNZHistory_.clear();
        actualValues_.clear();
        actualValuesSet_.clear();
        weightMatrix_.clear();

        for (auto step : proto.getSteps())
        {
          steps_.push_back(step);
        }

        alpha_ = proto.getAlpha();
        actValueAlpha_ = proto.getActValueAlpha();
        learnIteration_ = proto.getLearnIteration();
        recordNumMinusLearnIteration_ = proto.getRecordNumMinusLearnIteration();
        recordNumMinusLearnIterationSet_ =
          proto.getRecordNumMinusLearnIterationSet();
        maxSteps_ = proto.getMaxSteps();

        auto patternNZHistoryProto = proto.getPatternNZHistory();
        for (UInt i = 0; i < patternNZHistoryProto.size(); i++)
        {
          patternNZHistory_.emplace_back(patternNZHistoryProto[i].size());
          for (UInt j = 0; j < patternNZHistoryProto[i].size(); j++)
          {
            patternNZHistory_[i][j] = patternNZHistoryProto[i][j];
          }
        }

        auto iterationNumHistoryProto = proto.getIterationNumHistory();
        for (UInt i = 0; i < iterationNumHistoryProto.size(); i++)
        {
          iterationNumHistory_.push_back(iterationNumHistoryProto[i]);
        }

        maxBucketIdx_ = proto.getMaxBucketIdx();
        maxInputIdx_ = proto.getMaxInputIdx();

        auto weightMatrixProto = proto.getWeightMatrix();
        for (UInt i = 0; i < weightMatrixProto.size(); ++i)
        {
          auto stepWeightMatrix = weightMatrixProto[i];
          UInt steps = stepWeightMatrix.getSteps();
          weightMatrix_[steps] = Matrix(maxInputIdx_, maxBucketIdx_);
          auto weights = stepWeightMatrix.getWeight();
          UInt j = 0;
          // un-flatten weight matrix, serialized as a list of floats
          for (UInt row = 0; row <= maxInputIdx_; ++row)
          {
	          for (UInt col = 0; col <= maxBucketIdx_; ++col)
	          {
	          	weightMatrix_[steps].at(row, col) = weights[j];
	          	j++;
	          }
          }
        }

        for (auto actValue : proto.getActualValues())
        {
          actualValues_.push_back(actValue);
        }

        for (auto actValueSet : proto.getActualValuesSet())
        {
          actualValuesSet_.push_back(actValueSet);
        }

        version_ = proto.getVersion();
        verbosity_ = proto.getVerbosity();
      }

			bool SDRClassifier::operator==(const SDRClassifier& other) const
			{
			  if (steps_.size() != other.steps_.size())
			  {
			    return false;
			  }
			  for (UInt i = 0; i < steps_.size(); i++)
			  {
			    if (steps_.at(i) != other.steps_.at(i))
			    {
			      return false;
			    }
			  }

			  if (fabs(alpha_ - other.alpha_) > 0.000001 ||
			      fabs(actValueAlpha_ - other.actValueAlpha_) > 0.000001 ||
			      learnIteration_ != other.learnIteration_ ||
			      recordNumMinusLearnIteration_ !=
			          other.recordNumMinusLearnIteration_  ||
			      recordNumMinusLearnIterationSet_ !=
			          other.recordNumMinusLearnIterationSet_  ||
			      maxSteps_ != other.maxSteps_)
			  {
			    return false;
			  }

			  if (patternNZHistory_.size() != other.patternNZHistory_.size())
			  {
			    return false;
			  }
			  for (UInt i = 0; i < patternNZHistory_.size(); i++)
			  {
			    if (patternNZHistory_.at(i).size() !=
			        other.patternNZHistory_.at(i).size())
			    {
			      return false;
			    }
			    for (UInt j = 0; j < patternNZHistory_.at(i).size(); j++)
			    {
			      if (patternNZHistory_.at(i).at(j) !=
			          other.patternNZHistory_.at(i).at(j))
			      {
			        return false;
			      }
			    }
			  }

			  if (iterationNumHistory_.size() !=
			      other.iterationNumHistory_.size())
			  {
			    return false;
			  }
			  for (UInt i = 0; i < iterationNumHistory_.size(); i++)
			  {
			    if (iterationNumHistory_.at(i) !=
			        other.iterationNumHistory_.at(i))
			    {
			      return false;
			    }
			  }

			  if (maxBucketIdx_ != other.maxBucketIdx_)
			  {
			    return false;
			  }

			  if (maxInputIdx_ != other.maxInputIdx_)
			  {
			  	return false;
			  }

			  if (weightMatrix_.size() != other.weightMatrix_.size())
			  {
			  	return false;
			  }
			  for (auto it = weightMatrix_.begin(); it != weightMatrix_.end(); it++)
			  {
			  	Matrix thisWeights = it->second;
			  	Matrix otherWeights = other.weightMatrix_.at(it->first);
			    for (UInt i = 0; i <= maxInputIdx_; ++i)
			    {
			    	for (UInt j = 0; j <= maxBucketIdx_; ++j)
			    	{
			    		if (thisWeights.at(i, j) != otherWeights.at(i, j))
			    		{
			    			return false;
			    		}
			    	}
			    }
			  }

			  if (actualValues_.size() != other.actualValues_.size() ||
			      actualValuesSet_.size() != other.actualValuesSet_.size())
			  {
			    return false;
			  }
			  for (UInt i = 0; i < actualValues_.size(); i++)
			  {
			    if (fabs(actualValues_[i] - other.actualValues_[i]) > 0.000001 ||
			        actualValuesSet_[i] != other.actualValuesSet_[i])
			    {
			      return false;
			    }
			  }

			  if (version_ != other.version_ ||
			      verbosity_ != other.verbosity_)
			  {
			    return false;
			  }

			  return true;
			}

	  } 	 
	}
}
