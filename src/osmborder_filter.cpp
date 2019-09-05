/*

  Copyright 2012-2016 Jochen Topf <jochen@topf.org>.
  Copyright 2016 Paul Norman <penorman@mac.com>.
  Copyright 2019 S Roychowdhury <sroycode@gmail.com>

  This file is part of OSMBorder, and is based on osmborder_filter.cpp
  from OSMCoastline

  OSMBorder is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OSMBorder is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OSMBorder.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <cstdlib>
#include <getopt.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <map>
#include <sstream>
#include <utility>   // for std::move

#include <osmium/io/any_compression.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/io/error.hpp>
#include <osmium/io/file.hpp>
#include <osmium/io/header.hpp>
#include <osmium/io/input_iterator.hpp>
#include <osmium/io/output_iterator.hpp>
#include <osmium/io/pbf_output.hpp>
#include <osmium/osm/box.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/node_ref.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/util/memory.hpp>
#include <osmium/util/verbose_output.hpp>
// for buffered
#include <osmium/builder/osm_object_builder.hpp>
// #include <osmium/handler.hpp>
// #include <osmium/visitor.hpp>

#include "return_codes.hpp"
#include "json.hpp"

void print_help()
{
	std::cout
	        << "osmborder_filter [OPTIONS] OSMFILE\n"
	        << "\nOptions:\n"
	        << "  -h, --help           - This help message\n"
	        << "  -o, --output=OSMFILE - Where to write output (default: none)\n"
	        << "  -v, --verbose        - Verbose output\n"
	        << "  -V, --version        - Show version and exit\n"
	        << "  -c, --changefile     - Change these relations and ways\n"
	        << "\n";
}

using json = nlohmann::json;

using idset = std::unordered_set<osmium::object_id_type>;
using strmap = std::map<std::string,std::string>;
using idmap = std::unordered_map<osmium::object_id_type,strmap>;

// populates required and blanks on exception
bool jsonize(const char* inp, idmap &waymap, idset &yesborder, idset &noborder)
{
	try {
		json jout;
		{
			std::ifstream i(inp);
			i>>jout;
		}
		auto ro=jout["relations"];
		if (ro.is_array()) {
			for (json::iterator it = ro.begin(); it != ro.end(); ++it) {
				if (!it->is_object()) continue;
				auto osm_id_ptr = it->find("osm_id");
				if (osm_id_ptr==it->end()) continue;
				auto wt_ptr = it->find("whitelist");
				if (wt_ptr!=it->end() && (wt_ptr->get<bool>())) yesborder.insert(osm_id_ptr->get<long>() );
				auto bk_ptr = it->find("blacklist");
				if (bk_ptr!=it->end() && (bk_ptr->get<bool>())) noborder.insert(osm_id_ptr->get<long>() );
			}
		}
		auto wo=jout["ways"];
		if (wo.is_array()) {
			for (json::iterator it = wo.begin(); it != wo.end(); ++it) {
				if (!it->is_object()) continue;
				auto osm_id_ptr = it->find("osm_id");
				if (osm_id_ptr==it->end()) continue;

				strmap smap;
				for (json::iterator jt = it->begin(); jt != it->end(); ++jt) {
					if (jt.key()!="osm_id" && jt->is_string())
						smap[jt.key()]=jt->get<std::string>();
				}
				waymap[osm_id_ptr->get<long>()]=smap;
			}
		}
		return true;

	}
	catch (std::exception& e) {
		yesborder.clear();
		noborder.clear();
		waymap.clear();
		return false;
	}
}

class RewriteHandler {

	osmium::memory::Buffer& m_buffer;

	// copy existing tags
	void copy_tags(osmium::builder::Builder& parent, const osmium::TagList& tags)
	{
		osmium::builder::TagListBuilder builder{parent};
		for (const auto& tag : tags) builder.add_tag(tag);
	}

	// copy existing and overwrite with supplied
	void copy_tags(osmium::builder::Builder& parent, const osmium::TagList& tags, const strmap& tagmap)
	{
		osmium::builder::TagListBuilder builder{parent};
		for (const auto& tag : tags) builder.add_tag(tag);
		for (const auto& tag : tagmap) builder.add_tag(tag.first, tag.second);
	}

public:
	explicit RewriteHandler(osmium::memory::Buffer& buffer) : m_buffer(buffer) { }

	// The node handler common
	void node(const osmium::Node& node)
	{
		{
			osmium::builder::NodeBuilder builder{m_buffer};
			builder.set_id(node.id());
			builder.set_location(node.location());
			copy_tags(builder, node.tags());
		}
		m_buffer.commit();
	}

	// The way handler for unchanged
	void way(const osmium::Way& way)
	{
		{
			osmium::builder::WayBuilder builder{m_buffer};
			builder.set_id(way.id());
			copy_tags(builder, way.tags());
			builder.add_item(way.nodes());
		}
		m_buffer.commit();
	}

	// The way handler for changed
	void way(const osmium::Way& way, const strmap& tagmap)
	{
		{
			osmium::builder::WayBuilder builder{m_buffer};
			builder.set_id(way.id());
			copy_tags(builder, way.tags(),tagmap);
			builder.add_item(way.nodes());
		}
		m_buffer.commit();
	}

	// The relation handler
	void relation(const osmium::Relation& relation)
	{
		{
			osmium::builder::RelationBuilder builder{m_buffer};
			builder.set_id(relation.id());
			copy_tags(builder, relation.tags());
			builder.add_item(relation.members());
		}
		m_buffer.commit();
	}

}; // class RewriteHandler

int main(int argc, char *argv[])
{
	std::string output_filename;
	bool verbose = false;
	/// border yes
	idset yesborder;
	/// border no
	idset noborder;
	/// ways to change
	idmap waymap;
	bool no_json_err=true;

	static struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{"output", required_argument, 0, 'o'},
		{"verbose", no_argument, 0, 'v'},
		{"version", no_argument, 0, 'V'},
		{"changefile", optional_argument, 0, 'c'},
		{0, 0, 0, 0}
	};

	while (1) {
		int c = getopt_long(argc, argv, "ho:vVc:", long_options, 0);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_help();
			std::exit(return_code_ok);
		case 'o':
			output_filename = optarg;
			break;
		case 'v':
			verbose = true;
			break;
		case 'c':
			no_json_err=jsonize(optarg,waymap,yesborder,noborder);
			break;
		case 'V':
			std::cout
			        << "osmborder_filter version " OSMBORDER_VERSION "\n"
			        << "Copyright (C) 2012-2016  Jochen Topf <jochen@topf.org>\n"
			        << "License: GNU GENERAL PUBLIC LICENSE Version 3 "
			        "<http://gnu.org/licenses/gpl.html>.\n"
			        << "This is free software: you are free to change and "
			        "redistribute it.\n"
			        << "There is NO WARRANTY, to the extent permitted by law.\n";
			std::exit(return_code_ok);
		default:
			std::exit(return_code_fatal);
		}
	}

	// The vout object is an output stream we can write to instead of
	// std::cerr. Nothing is written if we are not in verbose mode.
	// The running time will be prepended to output lines.
	osmium::util::VerboseOutput vout(verbose);

	if (!no_json_err) {
		std::cerr << "changefile gave error, not using\n";
		// std::exit(return_code_cmdline);
	}

	if (output_filename.empty()) {
		std::cerr << "Missing -o/--output=OSMFILE option\n";
		std::exit(return_code_cmdline);
	}

	if (optind != argc - 1) {
		std::cerr << "Usage: osmborder_filter [OPTIONS] OSMFILE\n";
		std::exit(return_code_cmdline);
	}

	osmium::io::Header header;
	header.set("generator", "osmborder_filter");
	header.add_box(osmium::Box{-180.0, -90.0, 180.0, 90.0});

	osmium::io::File infile{argv[optind]};

	try {
		osmium::io::Writer writer{output_filename, header, osmium::io::overwrite::allow};
		osmium::memory::Buffer output_buffer{1024, osmium::memory::Buffer::auto_grow::yes};
		RewriteHandler handler{output_buffer};

		auto output_it = osmium::io::make_output_iterator(writer);

		std::vector<osmium::object_id_type> way_ids;
		std::vector<osmium::object_id_type> node_ids;

		vout << "Reading relations (1st pass through input file)...\n";
		{
			osmium::io::Reader reader{infile, osmium::osm_entity_bits::relation};
			auto relations =
			    osmium::io::make_input_iterator_range<const osmium::Relation>(
			        reader);
			for (const osmium::Relation &relation : relations) {
				if (noborder.find(relation.id()) != noborder.end() ) {
					vout << "Rejected relation: " << relation.id() << " ..\n";
					continue;
				}
				bool add_anyway = (yesborder.find(relation.id()) != yesborder.end() ) ;
				if (add_anyway && (!relation.tags().has_tag("boundary", "administrative"))) {
					vout << "Added relation: " << relation.id() << " ..\n";
				}
				if (add_anyway || relation.tags().has_tag("boundary", "administrative")) {
					*output_it++ = relation;
					for (const auto &rm : relation.members()) {
						if (rm.type() == osmium::item_type::way) {
							way_ids.push_back(rm.ref());
						}
					}
				}
			}
			reader.close();
		}

		vout << "Preparing way ID list...\n";
		std::sort(way_ids.begin(), way_ids.end());

		vout << "Reading ways (2nd pass through input file)...\n";

		{
			osmium::io::Reader reader{infile, osmium::osm_entity_bits::way};
			auto ways =
			    osmium::io::make_input_iterator_range<const osmium::Way>(reader);

			auto first = way_ids.begin();
			auto last = std::unique(way_ids.begin(), way_ids.end());

			for (const osmium::Way &way : ways) {
				// Advance the target list to the first possible way
				while (*first < way.id() && first != last) {
					++first;
				}

				if (way.id() == *first) {
					// *output_it++ = way;
					// start changes for way
					auto nway_it = waymap.find(way.id());
					if (nway_it == waymap.end() ) handler.way(way);
					else handler.way(way,nway_it->second);
					// end changes for way
					for (const auto &nr : way.nodes()) {
						node_ids.push_back(nr.ref());
					}
					if (first != last) {
						++first;
					}
				}
			}
			writer(std::move(output_buffer));
		}


		vout << "Preparing node ID list...\n";
		std::sort(node_ids.begin(), node_ids.end());
		auto last = std::unique(node_ids.begin(), node_ids.end());

		vout << "Reading nodes (3rd pass through input file)...\n";
		{
			osmium::io::Reader reader{infile, osmium::osm_entity_bits::node};
			auto nodes = osmium::io::make_input_iterator_range<const osmium::Node>( reader);

			auto first = node_ids.begin();
			std::copy_if(nodes.cbegin(), nodes.cend(), output_it,
			[&first, &last](const osmium::Node &node) {
				while (*first < node.id() && first != last) {
					++first;
				}
				if (node.id() == *first) {
					if (first != last) {
						++first;
					}
					return true;
				}
				return false;
			});

			reader.close();
		}
		writer.close();
	}
	catch (const osmium::io_error &e) {
		std::cerr << "io error: " << e.what() << "'\n";
		std::exit(return_code_fatal);
	}

	vout << "All done.\n";
	osmium::MemoryUsage mem;
	if (mem.current() > 0) {
		vout << "Memory used: current: " << mem.current() << " MBytes\n"
		     << "             peak:    " << mem.peak() << " MBytes\n";
	}
}
