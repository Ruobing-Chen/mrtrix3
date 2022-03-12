/* Copyright (c) 2008-2022 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Covered Software is provided under this License on an "as is"
 * basis, without warranty of any kind, either expressed, implied, or
 * statutory, including, without limitation, warranties that the
 * Covered Software is free of defects, merchantable, fit for a
 * particular purpose or non-infringing.
 * See the Mozilla Public License v. 2.0 for more details.
 *
 * For more details, see http://www.mrtrix.org/.
 */

#include "command.h"
#include "image.h"
#include "image_helpers.h"
#include "memory.h"
#include "thread.h"
#include "ordered_thread_queue.h"
#include "dwi/tractography/file.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/scalar_file.h"
#include "dwi/tractography/mapping/mapper.h"
#include "file/ofstream.h"
#include "file/path.h"
#include "interp/linear.h"
#include "interp/nearest.h"
#include "math/median.h"




using namespace MR;
using namespace App;


enum stat_tck { MEAN, MEDIAN, MIN, MAX, NONE };
const char* statistics[] = { "mean", "median", "min", "max", nullptr };

enum interp_type { NEAREST, LINEAR, PRECISE };


void usage ()
{
  AUTHOR = "Robert E. Smith (robert.smith@florey.edu.au)";

  SYNOPSIS = "Sample values of an associated image along tracks";

  DESCRIPTION
  + "By default, the value of the underlying image at each point along the track "
    "is written to either an ASCII file (with all values for each track on the same "
    "line), or a track scalar file (.tsf). Alternatively, some statistic can be "
    "taken from the values along each streamline and written to a vector file, "
    "which can either be in the NumPy .npy format or a numerical text file."

  + "In the circumstance where a per-streamline statistic is requested, the input "
    "image can be 4D rather than 3D; in that circumstance, each volume will be sampled "
    "independently, and the output (whether in .npy or text format) will be a matrix, "
    "with one row per streamline and one column per metric.";

  ARGUMENTS
  + Argument ("tracks", "the input track file").type_tracks_in()
  + Argument ("image",  "the image to be sampled").type_image_in()
  + Argument ("values", "the output sampled values").type_file_out();

  OPTIONS

  + Option ("stat_tck", "compute some statistic from the values along each streamline "
                        "(options are: " + join(statistics, ",") + ")")
    + Argument ("statistic").type_choice (statistics)

  + Option ("nointerp", "do not use trilinear interpolation when sampling image values")

  + Option ("precise", "use the precise mechanism for mapping streamlines to voxels "
                       "(obviates the need for trilinear interpolation) "
                       "(only applicable if some per-streamline statistic is requested)")

  + Option ("use_tdi_fraction",
            "each streamline is assigned a fraction of the image intensity "
            "in each voxel based on the fraction of the track density "
            "contributed by that streamline (this is only appropriate for "
            "processing a whole-brain tractogram, and images for which the "
            "quantiative parameter is additive)");


  // TODO add support for SH amplitude along tangent
  // TODO add support for reading from fixel image
  //   (this would supersede fixel2tsf when used without -precise or -stat_tck options)
  //   (wait until fixel_twi is merged; should simplify)

  REFERENCES
    + "* If using -precise option: " // Internal
    "Smith, R. E.; Tournier, J.-D.; Calamante, F. & Connelly, A. "
    "SIFT: Spherical-deconvolution informed filtering of tractograms. "
    "NeuroImage, 2013, 67, 298-312";


}



using value_type = float;
using vector_type = Eigen::VectorXf;
using matrix_type = Eigen::MatrixXf;


struct Statistic3D { NOMEMALIGN
  value_type value;
  size_t index;
};
struct Statistics4D { MEMALIGN(Statistics4D)
  vector_type values;
  size_t index;
};



class TDI { MEMALIGN(TDI)
  public:
    TDI (Image<value_type>& image, const size_t num_tracks) :
        image (image),
        progress ("Generating initial TDI", num_tracks) { }
    ~TDI () { progress.done(); }

    bool operator() (const DWI::Tractography::Mapping::SetVoxel& in)
    {
      for (const auto& v : in) {
        assign_pos_of (v, 0, 3).to (image);
        image.value() += v.get_length();
      }
      ++progress;
      return true;
    }

  protected:
    Image<value_type>& image;
    ProgressBar progress;

};



template <class Interp>
class SamplerNonPrecise
{ MEMALIGN (SamplerNonPrecise<Interp>)
  public:
    SamplerNonPrecise (Image<value_type>& image, const stat_tck statistic, const Image<value_type>& precalc_tdi) :
        interp (image),
        mapper (precalc_tdi.valid() ? new DWI::Tractography::Mapping::TrackMapperBase (image) : nullptr),
        tdi (precalc_tdi),
        statistic (statistic)
    {
      if (mapper)
        mapper->set_use_precise_mapping (false);
    }

    bool operator() (const DWI::Tractography::Streamline<value_type>& tck, Statistics4D& out)
    {
      assert (statistic != stat_tck::NONE);
      out.index = tck.get_index();
      matrix_type values;
      (*this) (tck, values);
      vector<value_type> weights (statistic == stat_tck::MEAN ? compute_weights (tck) : vector<value_type>());
      out.values.resize (interp.size(3));
      for (size_t i = 0; i != interp.size(3); ++i)
        out.values[i] = compute_statistic (values.col(i), weights);
      return true;
    }

    bool operator() (const DWI::Tractography::Streamline<value_type>& tck, Statistic3D& out)
    {
      assert (statistic != stat_tck::NONE);
      out.index = tck.get_index();
      DWI::Tractography::TrackScalar<value_type> values;
      (*this) (tck, values);
      vector<value_type> weights (statistic == stat_tck::MEAN ? compute_weights (tck) : vector<value_type>());
      out.value = compute_statistic (values, weights);
      return true;
    }

    bool operator() (const DWI::Tractography::Streamline<value_type>& tck,
                     DWI::Tractography::TrackScalar<value_type>& out)
    {
      out.set_index (tck.get_index());
      out.resize (tck.size());
      for (size_t i = 0; i != tck.size(); ++i) {
        if (interp.scanner (tck[i]))
          out[i] = interp.value();
        else
          out[i] = NAN;
      }
      return true;
    }

    bool operator() (const DWI::Tractography::Streamline<value_type>& tck,
                     matrix_type& out)
    {
      assert (interp.ndim() == 4);
      out.resize (tck.size(), interp.size(3));
      for (size_t i = 0; i != tck.size(); ++i) {
        if (interp.scanner (tck[i])) {
          for (auto l = Loop(3) (interp); l; ++l)
            out(i, interp.index(3)) = interp.value();
        } else {
          out.row(i).setConstant (NAN);
        }
      }
      return true;
    }

  private:
    Interp interp;
    std::shared_ptr<DWI::Tractography::Mapping::TrackMapperBase> mapper;
    Image<value_type> tdi;
    const stat_tck statistic;

    value_type get_tdi_multiplier (const DWI::Tractography::Mapping::Voxel& v)
    {
      if (!tdi.valid())
        return value_type(1);
      assign_pos_of (v).to (tdi);
      assert (!is_out_of_bounds (tdi));
      return v.get_length() / tdi.value();
    }

    // Take distance between points into account in mean calculation
    //   (Should help down-weight endpoints)
    vector<value_type> compute_weights (const DWI::Tractography::Streamline<value_type>& tck)
    {
      vector<value_type> weights;
      weights.reserve (tck.size());
      for (size_t i = 0; i != tck.size(); ++i) {
        value_type length = value_type(0);
        if (i)
          length += (tck[i] - tck[i-1]).norm();
        if (i < tck.size() - 1)
          length += (tck[i+1] - tck[i]).norm();
        weights.push_back (0.5 * length);
      }
      return weights;
    }


    template <class VectorType>
    value_type compute_statistic (const VectorType& data, const vector<value_type>& weights) const
    {
      switch (statistic) {
        case stat_tck::MEAN: {
          value_type integral = value_type(0), sum_weights = value_type(0);
          for (size_t i = 0; i != data.size(); ++i) {
            if (!std::isnan (data[i])) {
              integral += data[i] * weights[i];
              sum_weights += weights[i];
            }
          }
          return sum_weights ? (integral / sum_weights) : NAN;
        }
        case stat_tck::MEDIAN: {
          // Don't bother with a weighted median here
          vector<value_type> finite_data;
          finite_data.reserve (data.size());
          for (size_t i = 0; i != data.size(); ++i) {
            if (!std::isnan (data[i]))
              finite_data.push_back (data[i]);
          }
          return finite_data.size() ? Math::median (finite_data) : NAN;
        }
        break;
        case stat_tck::MIN: {
          value_type value = std::numeric_limits<value_type>::infinity();
          bool cast_to_nan = true;
          for (size_t i = 0; i != data.size(); ++i) {
            if (!std::isnan (data[i])) {
              value = std::min (value, data[i]);
              cast_to_nan = false;
            }
          }
          return cast_to_nan ? NAN : value;
        }
        break;
        case stat_tck::MAX: {
          value_type value = -std::numeric_limits<value_type>::infinity();
          bool cast_to_nan = true;
          for (size_t i = 0; i != data.size(); ++i) {
            if (!std::isnan (data[i])) {
              value = std::max (value, data[i]);
              cast_to_nan = false;
            }
          }
          return cast_to_nan ? NAN : value;
        }
        break;
        default: assert (0); return std::numeric_limits<value_type>::signaling_NaN();
      }
    }



};



class SamplerPrecise
{ MEMALIGN (SamplerPrecise)
  public:
    SamplerPrecise (Image<value_type>& image, const stat_tck statistic, const Image<value_type>& precalc_tdi) :
        image (image),
        mapper (new DWI::Tractography::Mapping::TrackMapperBase (image)),
        tdi (precalc_tdi),
        statistic (statistic)
    {
      assert (statistic != stat_tck::NONE);
      mapper->set_use_precise_mapping (true);
    }

    bool operator() (DWI::Tractography::Streamline<value_type>& tck, Statistics4D& out)
    {
      out.index = tck.get_index();
      DWI::Tractography::Mapping::SetVoxel voxels;
      (*mapper) (tck, voxels);
      out.values.resize (image.size(3));
      for (auto l = Loop(3) (image); l; ++l)
        out.values[image.index(3)] = compute_statistic (voxels);
      return true;
    }

    bool operator() (DWI::Tractography::Streamline<value_type>& tck, Statistic3D& out)
    {
      out.index = tck.get_index();
      DWI::Tractography::Mapping::SetVoxel voxels;
      (*mapper) (tck, voxels);
      out.value = compute_statistic (voxels);
      return true;
    }


  private:
    Image<value_type> image;
    std::shared_ptr<DWI::Tractography::Mapping::TrackMapperBase> mapper;
    Image<value_type> tdi;
    const stat_tck statistic;

    value_type get_tdi_multiplier (const DWI::Tractography::Mapping::Voxel& v)
    {
      if (!tdi.valid())
        return value_type(1);
      assign_pos_of (v).to (tdi);
      assert (!is_out_of_bounds (tdi));
      return v.get_length() / tdi.value();
    }

    value_type compute_statistic (const DWI::Tractography::Mapping::SetVoxel& voxels)
    {
      switch (statistic) {
        case stat_tck::MEAN: {
          value_type integral = value_type(0), sum_lengths = value_type(0);
          for (const auto& v : voxels) {
            assign_pos_of (v, 0, 3).to (image);
            const value_type value = image.value();
            if (std::isfinite (value)) {
              integral += v.get_length() * (value * get_tdi_multiplier (v));
              sum_lengths += v.get_length();
            }
          }
          return sum_lengths ? (integral / sum_lengths) : NAN;
        }
        case stat_tck::MEDIAN: {
          // Should be a weighted median...
          // Just use the n.log(n) algorithm
          class WeightSort { NOMEMALIGN
            public:
              WeightSort (const DWI::Tractography::Mapping::Voxel& voxel, const value_type value) :
                value (value),
                length (voxel.get_length()) { }
              bool operator< (const WeightSort& that) const { return value < that.value; }
              value_type value, length;
          };
          vector<WeightSort> data;
          value_type sum_lengths (value_type(0));
          for (const auto& v : voxels) {
            assign_pos_of (v).to (image);
            const value_type value = image.value();
            if (std::isfinite (value)) {
              data.push_back (WeightSort (v, (image.value() * get_tdi_multiplier (v))));
              sum_lengths += v.get_length();
            }
          }
          if (!data.size())
            return NAN;
          std::sort (data.begin(), data.end());
          const value_type target_length = 0.5 * sum_lengths;
          sum_lengths = value_type(0);
          value_type prev_value = data.front().value;
          for (const auto& d : data) {
            if ((sum_lengths += d.length) > target_length)
              return prev_value;
            prev_value = d.value;
          }
          assert (0);
          return std::numeric_limits<value_type>::signaling_NaN();
        }
        case stat_tck::MIN: {
          value_type minvalue = std::numeric_limits<value_type>::infinity();
          bool cast_to_nan = true;
          for (const auto& v : voxels) {
            assign_pos_of (v).to (image);
            const value_type value = image.value();
            if (!std::isnan (value)) {
              minvalue = std::min (minvalue, value * get_tdi_multiplier (v));
              cast_to_nan = false;
            }
          }
          return cast_to_nan ? NAN : minvalue;
        }
        case stat_tck::MAX: {
          value_type maxvalue = -std::numeric_limits<value_type>::infinity();
          bool cast_to_nan = true;
          for (const auto& v : voxels) {
            assign_pos_of (v).to (image);
            const value_type value = image.value();
            if (!std::isnan (value)) {
              maxvalue = std::max (maxvalue, value * get_tdi_multiplier (v));
              cast_to_nan = false;
            }
          }
          return cast_to_nan ? NAN : maxvalue;
        }
        default:
          assert (0);
          return std::numeric_limits<value_type>::signaling_NaN();
      }
    }

};



class ReceiverBase { MEMALIGN(ReceiverBase)
  public:
    ReceiverBase (const size_t num_tracks) :
        received (0),
        expected (num_tracks),
        progress ("Sampling values underlying streamlines", num_tracks) { }

    ReceiverBase (const ReceiverBase&) = delete;

    virtual ~ReceiverBase() {
      if (received != expected)
        WARN ("Track file reports " + str(expected) + " tracks, but contains " + str(received));
    }

  protected:
    void operator++ () {
      ++received;
      ++progress;
    }

    size_t received;

  private:
    const size_t expected;
    ProgressBar progress;

};


class Receiver_Statistic3D : private ReceiverBase { MEMALIGN(Receiver_Statistic3D)
  public:
    Receiver_Statistic3D (const size_t num_tracks) :
        ReceiverBase (num_tracks),
        data (vector_type::Zero (num_tracks)) { }
    Receiver_Statistic3D (const Receiver_Statistic3D&) = delete;

    bool operator() (Statistic3D& in) {
      if (in.index >= size_t(data.size()))
        data.conservativeResizeLike (vector_type::Zero (in.index + 1));
      data[in.index] = in.value;
      ++(*this);
      return true;
    }

    void save (const std::string& path) {
      MR::save_vector (data, path);
    }

  private:
    vector_type data;
};


class Receiver_Statistics4D : private ReceiverBase { MEMALIGN(Receiver_Statistics4D)
  public:
    Receiver_Statistics4D (const size_t num_tracks, const size_t num_metrics) :
        ReceiverBase (num_tracks),
        data (matrix_type::Zero (num_tracks, num_metrics)) { }
    Receiver_Statistics4D (const Receiver_Statistics4D&) = delete;

    bool operator() (Statistics4D& in) {
      // TODO Chance that this will be prohibitively slow if count is not indicated in track file header
      if (in.index >= size_t(data.rows()))
        data.conservativeResizeLike (matrix_type::Zero (in.index + 1, data.cols()));
      data.row(in.index) = in.values;
      ++(*this);
      return true;
    }

    void save (const std::string& path) {
      MR::save_matrix (data, path);
    }

  private:
    matrix_type data;
};



class Receiver_NoStatistic : private ReceiverBase { MEMALIGN(Receiver_NoStatistic)
  public:
    Receiver_NoStatistic (const std::string& path,
                          const size_t num_tracks,
                          const DWI::Tractography::Properties& properties) :
        ReceiverBase (num_tracks)
    {
      if (Path::has_suffix (path, ".tsf")) {
        tsf.reset (new DWI::Tractography::ScalarWriter<value_type> (path, properties));
      } else {
        ascii.reset (new File::OFStream (path));
        (*ascii) << "# " << App::command_history_string << "\n";
      }
    }
    Receiver_NoStatistic (const Receiver_NoStatistic&) = delete;

    bool operator() (const DWI::Tractography::TrackScalar<value_type>& in)
    {
      // Requires preservation of order
      assert (in.get_index() == ReceiverBase::received);
      if (ascii) {
        if (in.size()) {
          auto i = in.begin();
          (*ascii) << *i;
          for (++i; i != in.end(); ++i)
            (*ascii) << " " << *i;
        }
        (*ascii) << "\n";
      } else {
        (*tsf) (in);
      }
      ++(*this);
      return true;
    }

  private:
    std::unique_ptr<File::OFStream> ascii;
    std::unique_ptr<DWI::Tractography::ScalarWriter<value_type>> tsf;
};




template <class InterpType>
void execute_nostat (DWI::Tractography::Reader<value_type>& reader,
                     const DWI::Tractography::Properties& properties,
                     const size_t num_tracks,
                     Image<value_type>& image,
                     const std::string& path)
{
  SamplerNonPrecise<InterpType> sampler (image, stat_tck::NONE, Image<value_type>());
  Receiver_NoStatistic receiver (path, num_tracks, properties);
  Thread::run_ordered_queue (reader,
                             Thread::batch (DWI::Tractography::Streamline<value_type>()),
                             Thread::multi (sampler),
                             Thread::batch (DWI::Tractography::TrackScalar<value_type>()),
                             receiver);
}

template <class SamplerType>
void execute (DWI::Tractography::Reader<value_type>& reader,
              const size_t num_tracks,
              Image<value_type>& image,
              const stat_tck statistic,
              Image<value_type>& tdi,
              const std::string& path)
{
  SamplerType sampler (image, statistic, tdi);
  const size_t num_metrics = image.ndim() == 4 ? image.size(3) : 1;
  if (num_metrics == 1) {
    Receiver_Statistic3D receiver (num_tracks);
    Thread::run_ordered_queue (reader,
                              Thread::batch (DWI::Tractography::Streamline<value_type>()),
                              Thread::multi (sampler),
                              Thread::batch (Statistic3D()),
                              receiver);
    receiver.save (path);
  } else {
    Receiver_Statistics4D receiver (num_tracks, num_metrics);
    Thread::run_ordered_queue (reader,
                              Thread::batch (DWI::Tractography::Streamline<value_type>()),
                              Thread::multi (sampler),
                              Thread::batch (Statistics4D()),
                              receiver);
    receiver.save (path);
  }
}



void run ()
{
  DWI::Tractography::Properties properties;
  DWI::Tractography::Reader<value_type> reader (argument[0], properties);
  auto H = Header::open (argument[1]);

  auto opt = get_options ("stat_tck");
  const stat_tck statistic = opt.size() ? stat_tck(int(opt[0][0])) : stat_tck::NONE;
  if (H.ndim() == 4 && H.size(3) != 1 && statistic != stat_tck::NONE) {
    INFO ("Input image is 4D; output will be 2D matrix");
  } else if (H.ndim() != 3) {
    throw Exception ("Input image is of unsupported dimensionality");
  }

  const bool nointerp = get_options ("nointerp").size();
  const bool precise = get_options ("precise").size();
  if (nointerp && precise)
    throw Exception ("Option -nointerp and -precise are mutually exclusive");
  const interp_type interp = nointerp ? interp_type::NEAREST : (precise ? interp_type::PRECISE : interp_type::LINEAR);
  const size_t num_tracks = properties.find("count") == properties.end() ?
                            0 :
                            to<size_t>(properties["count"]);

  if (statistic == stat_tck::NONE && interp == interp_type::PRECISE)
    throw Exception ("Precise streamline mapping may only be used with per-streamline statistics");

  Image<value_type> tdi;
  if (get_options ("use_tdi_fraction").size()) {
    if (statistic == stat_tck::NONE)
      throw Exception ("Cannot use -use_tdi_fraction option unless a per-streamline statistic is used");
    DWI::Tractography::Reader<value_type> tdi_reader (argument[0], properties);
    DWI::Tractography::Mapping::TrackMapperBase mapper (H);
    mapper.set_use_precise_mapping (interp == interp_type::PRECISE);
    tdi = Image<value_type>::scratch (H, "TDI scratch image");
    TDI tdi_fill (tdi, num_tracks);
    Thread::run_queue (tdi_reader,
                       Thread::batch (DWI::Tractography::Streamline<value_type>()),
                       Thread::multi (mapper),
                       Thread::batch (DWI::Tractography::Mapping::SetVoxel()),
                       tdi_fill);
  }

  auto image = H.get_image<value_type>();
  if (statistic == stat_tck::NONE) {
    switch (interp) {
      case interp_type::NEAREST:
        execute_nostat<Interp::Nearest<Image<value_type>>> (reader, properties, num_tracks, image, argument[2]);
        break;
      case interp_type::LINEAR:
        execute_nostat<Interp::Linear<Image<value_type>>> (reader, properties, num_tracks, image, argument[2]);
        break;
      case interp_type::PRECISE:
        throw Exception ("Precise streamline mapping may only be used with per-streamline statistics");
    }
  } else {
    switch (interp) {
      case interp_type::NEAREST:
        execute<SamplerNonPrecise<Interp::Nearest<Image<value_type>>>> (reader, num_tracks, image, statistic, tdi, argument[2]);
        break;
      case interp_type::LINEAR:
        execute<SamplerNonPrecise<Interp::Linear<Image<value_type>>>> (reader, num_tracks, image, statistic, tdi, argument[2]);
        break;
      case interp_type::PRECISE:
        execute<SamplerPrecise> (reader, num_tracks, image, statistic, tdi, argument[2]);
        break;
    }
  }
}

