/*
    Copyright 2011 Brain Research Institute, Melbourne, Australia

    Written by Robert E. Smith, 2012.

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


#include <string>
#include <vector>

#include "command.h"
#include "point.h"
#include "progressbar.h"
#include "memory.h"

#include "connectome/connectome.h"

#include "dwi/tractography/file.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/weights.h"
#include "dwi/tractography/connectome/extract.h"
#include "dwi/tractography/connectome/streamline.h"
#include "dwi/tractography/connectome/tck2nodes.h"
#include "dwi/tractography/mapping/loader.h"

#include "math/matrix.h"

#include "image/buffer.h"
#include "image/loop.h"
#include "image/nav.h"
#include "image/transform.h"
#include "image/voxel.h"

#include "thread_queue.h"




using namespace MR;
using namespace App;
using namespace MR::Connectome;
using namespace MR::DWI;
using namespace MR::DWI::Tractography;
using namespace MR::DWI::Tractography::Connectome;


const char* file_outputs[] = { "per_edge", "per_node", "single", NULL };


const OptionGroup OutputOptions = OptionGroup ("Options for determining the content / format of output files")

    + Option ("nodes", "only select tracks that involve a set of nodes of interest")
      + Argument ("list").type_sequence_int()

    + Option ("exclusive", "only select tracks that exclusively connect nodes of interest at both ends")

    + Option ("files", "select how the resulting streamlines will be grouped in output files. "
                       "Options are: per_edge, per_node, single (default: per_edge)")
      + Argument ("option").type_choice (file_outputs)

    + Option ("exemplars", "generate a mean connection exemplar per edge, rather than keeping all streamlines "
                           "(the parcellation node image must be provided in order to constrain the exemplar endpoints)")
      + Argument ("image").type_image_in()

    + Option ("keep_unassigned", "by default, the program discards those streamlines that are not successfully assigned to a node. "
                                 "Set this option to generate corresponding outputs containing these streamlines (labelled as node index 0)");


const OptionGroup TrackWeightsOptions = OptionGroup ("Options for importing / exporting streamline weights")

    + Tractography::TrackWeightsInOption

    + Option ("prefix_tck_weights_out", "provide a prefix for outputting a text file corresponding to each output file, "
                                      "each containing only the streamline weights relevant for that track file")
      + Argument ("prefix").type_text();



void usage ()
{

	AUTHOR = "Robert E. Smith (r.smith@brain.org.au)";

  DESCRIPTION
  + "extract streamlines from a tractogram based on their assignment to parcellated nodes";

  ARGUMENTS
  + Argument ("tracks_in",      "the input track file").type_file_in()
  + Argument ("assignments_in", "text file containing the node assignments for each streamline").type_file_in()
  + Argument ("prefix_out",     "the output file / prefix").type_text();


  OPTIONS
  + OutputOptions
  + TrackWeightsOptions;

};




void run ()
{

  Tractography::Properties properties;
  Tractography::Reader<float> reader (argument[0], properties);

  std::vector<NodePair> assignments;
  node_t max_node_index = 0;
  {
    std::ifstream stream (argument[1]);
    std::string line;
    NodePair temp;
    while (std::getline (stream, line)) {
      sscanf (line.c_str(), "%u %u", &temp.first, &temp.second);
      assignments.push_back (temp);
      max_node_index = maxvalue (max_node_index, temp.first, temp.second);
    }
  }
  const size_t count = to<size_t>(properties["count"]);
  if (assignments.size() != count)
    throw Exception ("Assignments file contains " + str(assignments.size()) + " entries; track file contains " + str(count) + " tracks");

  const std::string prefix (argument[2]);
  Options opt = get_options ("prefix_tck_weights_out");
  const std::string weights_prefix = opt.size() ? std::string (opt[0][0]) : "";

  INFO ("Maximum node index is " + str(max_node_index));

  const node_t first_node = get_options ("keep_unassigned").size() ? 0 : 1;

  // Get the list of nodes of interest
  std::vector<node_t> nodes;
  opt = get_options ("nodes");
  if (opt.size()) {
    std::vector<int> data = parse_ints (opt[0][0]);
    bool zero_in_list = false;
    for (std::vector<int>::const_iterator i = data.begin(); i != data.end(); ++i) {
      nodes.push_back (node_t (*i));
      if (!*i)
        zero_in_list = true;
    }
    if (!zero_in_list && !first_node)
      nodes.push_back (0);
    std::sort (nodes.begin(), nodes.end());
  } else {
    for (node_t i = first_node; i <= max_node_index; ++i)
      nodes.push_back (i);
  }

  const bool exclusive = get_options ("exclusive").size();

  opt = get_options ("files");
  const int file_format = opt.size() ? opt[0][0] : 0;

  opt = get_options ("exemplars");
  if (opt.size()) {

    // Load the node image, get the centres of mass
    // Generate exemplars - these can _only_ be done per edge, and requires a mutex per edge to multi-thread
    Image::Buffer<node_t> buffer (opt[0][0]);
    Image::Buffer<node_t>::voxel_type voxel (buffer);
    Image::Transform transform (buffer);

    std::vector< Point<float> > COMs (max_node_index+1, Point<float> (0.0, 0.0, 0.0));
    std::vector<size_t> volumes (max_node_index+1, 0);
    Image::LoopInOrder loop (voxel);
    for (auto i = loop (voxel); i; ++i) {
      const node_t index = voxel.value();
      if (index) {
        assert (index <= max_node_index);
        COMs[index] += Point<float> (voxel[0], voxel[1], voxel[2]);
        ++volumes[index];
      }
    }
    for (node_t index = 1; index <= max_node_index; ++index) {
      if (volumes[index])
        COMs[index] = transform.voxel2scanner (COMs[index] * (1.0f / float(volumes[index])));
      else
        COMs[index].invalidate();
    }

    // If user specifies a subset of nodes, only a subset of exemplars need to be calculated
    WriterExemplars generator (properties, nodes, exclusive, first_node, COMs);

    {
      std::mutex mutex;
      ProgressBar progress ("generating exemplars for connectome... ", count);
      auto loader = [&] (Tractography::Connectome::Streamline& out) { if (!reader (out)) return false; out.set_nodes (assignments[out.index]); return true; };
      auto worker = [&] (const Tractography::Connectome::Streamline& in) { generator (in); std::lock_guard<std::mutex> lock (mutex); ++progress; return true; };
      Thread::run_queue (loader, Tractography::Connectome::Streamline(), Thread::multi (worker));
    }

    generator.finalize();

    // Get exemplars to the output file(s), depending on the requested format
    if (file_format == 0) { // One file per edge
      if (exclusive) {
        ProgressBar progress ("writing exemplars to files... ", nodes.size() * (nodes.size()-1) / 2);
        for (size_t i = 0; i != nodes.size(); ++i) {
          const node_t one = nodes[i];
          for (size_t j = i+1; j != nodes.size(); ++j) {
            const node_t two = nodes[j];
            generator.write (one, two, prefix + str(one) + "-" + str(two) + ".tck", weights_prefix.size() ? (weights_prefix + str(one) + "-" + str(two) + ".csv") : "");
            ++progress;
          }
        }
      } else {
        // For each node in the list, write one file for an exemplar to every other node
        ProgressBar progress ("writing exemplars to files... ", nodes.size() * COMs.size());
        for (std::vector<node_t>::const_iterator n = nodes.begin(); n != nodes.end(); ++n) {
          for (size_t i = first_node; i != COMs.size(); ++i) {
            generator.write (*n, i, prefix + str(*n) + "-" + str(i) + ".tck", weights_prefix.size() ? (weights_prefix + str(*n) + "-" + str(i) + ".csv") : "");
            ++progress;
          }
        }
      }
    } else if (file_format == 1) { // One file per node
      ProgressBar progress ("writing exemplars to files... ", nodes.size());
      for (std::vector<node_t>::const_iterator n = nodes.begin(); n != nodes.end(); ++n) {
        generator.write (*n, prefix + str(*n) + ".tck", weights_prefix.size() ? (weights_prefix + str(*n) + ".csv") : "");
        ++progress;
      }
    } else if (file_format == 2) { // Single file
      std::string path = prefix;
      if (path.rfind (".tck") != path.size() - 4)
        path += ".tck";
      std::string weights_path = weights_prefix;
      if (weights_prefix.size() && weights_path.rfind (".tck") != weights_path.size() - 4)
        weights_path += ".csv";
      generator.write (path, weights_path);
    }

  } else { // Old behaviour ie. all tracks, rather than generating exemplars

    WriterExtraction writer (properties, nodes, exclusive);

    switch (file_format) {
      case 0: // One file per edge
        for (size_t i = 0; i != nodes.size(); ++i) {
          const node_t one = nodes[i];
          if (exclusive) {
            for (size_t j = i; j != nodes.size(); ++j) {
              const node_t two = nodes[j];
              writer.add (one, two, prefix + str(one) + "-" + str(two) + ".tck", weights_prefix.size() ? (weights_prefix + str(one) + "-" + str(two) + ".csv") : "");
            }
          } else {
            // Allow duplication of edges; want to have a set of files for each node
            for (node_t two = first_node; two <= max_node_index; ++two) {
              writer.add (one, two, prefix + str(one) + "-" + str(two) + ".tck", weights_prefix.size() ? (weights_prefix + str(one) + "-" + str(two) + ".csv") : "");
            }
          }
        }
        INFO ("A total of " + str (writer.file_count()) + " output track files will be generated (one for each edge)");
        break;
      case 1: // One file per node
        for (std::vector<node_t>::const_iterator i = nodes.begin(); i != nodes.end(); ++i)
          writer.add (*i, prefix + str(*i) + ".tck", weights_prefix.size() ? (weights_prefix + str(*i) + ".csv") : "");
        INFO ("A total of " + str (writer.file_count()) + " output track files will be generated (one for each node)");
        break;
      case 2: // Single file
        std::string path = prefix;
        if (path.rfind (".tck") != path.size() - 4)
          path += ".tck";
        std::string weights_path = weights_prefix;
        if (weights_prefix.size() && weights_path.rfind (".tck") != weights_path.size() - 4)
          weights_path += ".csv";
        writer.add (nodes, path, weights_path);
        break;
    }

    Tractography::Connectome::Streamline tck;
    ProgressBar progress ("Extracting tracks from connectome... ", count);
    while (reader (tck)) {
      tck.set_nodes (assignments[tck.index]);
      writer (tck);
      ++progress;
    }

  }

}