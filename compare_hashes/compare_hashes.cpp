#include <iostream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <string>
#include <cstdlib>
#include <fstream>
#include <set>
#include <iomanip>
#include <time.h>

#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"
#include "nlohmann/json.hpp"

#include "lichen_config.h"
#include "submission.h"
#include "hash_location.h"


// =============================================================================
// helper typedefs

typedef int location_in_submission;
typedef unsigned int hash;
typedef std::string user_id;
typedef unsigned int version_number;


// =============================================================================
// helper functions

// ensures that all of the regions in the two parameters are adjacent
bool matchingPositionsAreAdjacent(const nlohmann::json &first, const nlohmann::json &second) {
  // they can't all be adjacent if there are an unequal number between the two lists
  if (first["matchingpositions"].size() != second["matchingpositions"].size()) {
    return false;
  }

  nlohmann::json::const_iterator itr1 = first["matchingpositions"].begin();
  nlohmann::json::const_iterator itr2 = second["matchingpositions"].begin();
  // iterate over each matching submission (first and second are the same length
  // so we don't have to check for the end of second)
  for (; itr1 != first["matchingpositions"].end(); itr1++, itr2++) {
    if ((*itr1)["end"].get<int>() + 1 != (*itr2)["end"].get<int>()) {
      return false;
    }
  }
  return true;
}


// increments the end position for each of the matches in the json provided,
// merging overlapping regions where necessary
void incrementEndPositionsForMatches(nlohmann::json &others) {
  nlohmann::json::iterator itr = others.begin();
  for (; itr != others.end(); itr++) {
    nlohmann::json::iterator itr2 = (*itr)["matchingpositions"].begin();
    nlohmann::json::iterator itr3 = ++((*itr)["matchingpositions"].begin());
    for (; itr3 != (*itr)["matchingpositions"].end();) {
      if ((*itr2)["end"].get<int>() >= (*itr3)["start"]) {
        (*itr2)["end"] = (*itr3)["end"].get<int>();
        itr3 = (*itr)["matchingpositions"].erase(itr3);
      }
      else {
        (*itr2)["end"] = (*itr2)["end"].get<int>() + 1;
        itr2++;
        itr3++;
      }
    }
    (*itr2)["end"] = (*itr2)["end"].get<int>() + 1;
  }
}


// =============================================================================
// MAIN

int main(int argc, char* argv[]) {
  std::cout << "COMPARE HASHES:" << std::endl;
  fflush(stdout);
  time_t overall_start, overall_end;
  time(&overall_start);


  // ===========================================================================
  // load Lichen config data
  std::ifstream lichen_config_istr(boost::filesystem::path(boost::filesystem::system_complete(argv[0]).parent_path().parent_path() / "bin/lichen_config.json").string());
  assert(lichen_config_istr.good());
  nlohmann::json lichen_config = nlohmann::json::parse(lichen_config_istr);
  LichenConfig config;

  // ===========================================================================
  // load config info

  assert(argc == 2);
  std::string lichen_gradeable_path_str = argv[1];
  boost::filesystem::path lichen_gradeable_path = boost::filesystem::system_complete(lichen_gradeable_path_str);
  boost::filesystem::path config_file_json_path = lichen_gradeable_path / "config.json";

  std::ifstream istr(config_file_json_path.string());
  assert(istr.good());
  nlohmann::json config_file_json = nlohmann::json::parse(istr);

  config.semester = config_file_json.value("semester", "ERROR");
  config.course = config_file_json.value("course", "ERROR");
  config.gradeable = config_file_json.value("gradeable", "ERROR");
  config.hash_size = config_file_json.value("hash_size", 1);
  config.threshold = config_file_json.value("threshold", 5);

  // error checking, confirm there are hashes to work with
  boost::filesystem::path users_root_directory = lichen_gradeable_path / "users";
  if (!boost::filesystem::exists(users_root_directory) ||
      !boost::filesystem::is_directory(users_root_directory)) {
    std::cerr << "ERROR with directory " << users_root_directory << std::endl;
    exit(0);
  }

  // the file path where we expect to find the hashed instructor provided code file
  boost::filesystem::path provided_code_file = lichen_gradeable_path / "provided_code" / "hashes.txt";
  // if file exists in that location, the provided code mode is enabled.
  config.provided_code_enabled = boost::filesystem::exists(provided_code_file);
  // path to other gradeables' data
  boost::filesystem::path other_gradeables_dir = lichen_gradeable_path / "other_gradeables";


  // ===========================================================================
  // loop over all submissions and populate the all_hashes and all_submissions structures

  // Stores all the hashes and their locations across all submissions (sorted in "bins" of student names)
  std::unordered_map<hash, std::unordered_map<user_id, std::vector<HashLocation>>> all_hashes;
  // Stores all submissions
  std::vector<Submission*> all_submissions;
  // Stores all hashes from the instructor provided code
  std::unordered_set<hash> provided_code;
  // stores all hashes from other gradeables
  std::unordered_map<hash, std::unordered_map<user_id, std::vector<HashLocation>>> other_gradeables;
  // a map of "user_id:version" strings to the non-zero number of times their matching positions array was truncated
  std::unordered_map<std::string, int> matching_positions_truncations;


  time_t start, end;
  time(&start);

  if (config.provided_code_enabled) {
    // load the instructor provided code's hashes
    std::ifstream istr(provided_code_file.string());
    assert(istr.good());
    hash instructor_hash;
    while (istr >> instructor_hash) {
      provided_code.insert(instructor_hash);
    }
  }

  // load other gradeables' hashes
  // iterate over all other gradeables
  boost::filesystem::directory_iterator end_iter;
  for (boost::filesystem::directory_iterator other_gradeable_itr(other_gradeables_dir); other_gradeable_itr != end_iter; ++other_gradeable_itr) {
    boost::filesystem::path other_gradeable_path = other_gradeable_itr->path();
    assert (is_directory(other_gradeable_path));
    std::string other_gradeable_str = other_gradeable_itr->path().filename().string();

    // loop over every user
    for (boost::filesystem::directory_iterator other_user_itr(other_gradeable_path); other_user_itr != end_iter; ++other_user_itr) {
      boost::filesystem::path other_username_path = other_user_itr->path();
      assert (is_directory(other_username_path));
      std::string other_username = other_user_itr->path().filename().string();

      // loop over every version
      for (boost::filesystem::directory_iterator other_version_itr(other_username_path); other_version_itr != end_iter; ++other_version_itr) {
        boost::filesystem::path other_version_path = other_version_itr->path();
        assert (is_directory(other_version_path));
        std::string str_other_version = other_version_itr->path().filename().string();
        version_number other_version = std::stoi(str_other_version);
        assert (other_version > 0);

        // load the hashes from this submission from another gradeable
        boost::filesystem::path other_hash_file = other_version_path / "hashes.txt";
        std::ifstream istr(other_hash_file.string());
        assert(istr.good());
        std::string input_hash_str;
        int location = 0;
        while (istr >> input_hash_str) {
          hash input_hash = (unsigned int)(stoul(input_hash_str, 0, 16));
          location++;
          other_gradeables[input_hash][other_username].push_back(HashLocation(other_username, other_version, location, other_gradeable_str));
        }
      }
    }
  }


  // loop over all users
  for (boost::filesystem::directory_iterator dir_itr( users_root_directory ); dir_itr != end_iter; ++dir_itr) {
    boost::filesystem::path username_path = dir_itr->path();
    assert (is_directory(username_path));
    std::string username = dir_itr->path().filename().string();

    // loop over all versions
    for (boost::filesystem::directory_iterator username_itr( username_path ); username_itr != end_iter; ++username_itr) {
      boost::filesystem::path version_path = username_itr->path();
      assert (is_directory(version_path));
      std::string str_version = username_itr->path().filename().string();
      version_number version = std::stoi(str_version);
      assert (version > 0);

      // create a submission object and load to the main submissions structure
      Submission* curr_submission = new Submission(username, version, config);

      // load the hashes from this submission
      boost::filesystem::path hash_file = version_path;
      hash_file /= "hashes.txt";
      std::ifstream istr(hash_file.string());
      assert(istr.good());
      std::string input_hash_str;
      int location = 0;
      while (istr >> input_hash_str) {
        hash input_hash = (unsigned int)(stoul(input_hash_str, 0, 16));
        location++;
        all_hashes[input_hash][username].push_back(HashLocation(username, version, location, config.semester + "__" + config.course + "__" + config.gradeable));
        curr_submission->addHash(input_hash, location);
      }

      all_submissions.push_back(curr_submission);
    }
  }

  time(&end);
  double diff = difftime(end, start);
  std::cout << "Finished loading in " << diff  << " seconds" << std::endl;


  // ===========================================================================
  // THIS IS THE MAIN PLAGIARISM DETECTION ALGORITHM

  // Used to calculate current progress (printed to the log)
  int my_counter = 0;
  int my_percent = 0;
  time(&start);

  std::cout << "[0%                      25%                     50%                     75%                     100%]" << std::endl << "[";
  fflush(stdout);

  // walk over every Submission
  for (std::vector<Submission*>::iterator submission_itr = all_submissions.begin();
       submission_itr != all_submissions.end(); ++submission_itr) {

    // =========================================================================
    // FINDING THE MATCHES

    // walk over every hash in that submission
    std::vector<std::pair<hash, location_in_submission>>::const_iterator hash_itr = (*submission_itr)->getHashes().begin();
    for (; hash_itr != (*submission_itr)->getHashes().end(); ++hash_itr) {

      // if provided code was enabled, look for the submission hash in the provided code's hashes
      bool provided_match_found = false;
      if (config.provided_code_enabled) {
        std::unordered_set<hash>::iterator provided_match_itr = provided_code.find(hash_itr->first);
        if (provided_match_itr != provided_code.end()) {
          provided_match_found = true;
          // add provded match
          (*submission_itr)->addProvidedMatch(hash_itr->second);
        }
      }

      // if the hash doesn't match any of the provided code's hashes, try to find matches between other students
      if (!provided_match_found) {
        // look up that hash in the all_hashes table, loop over all other students that have the same hash
        std::unordered_map<std::string, std::vector<HashLocation>> occurences = all_hashes[hash_itr->first];
        std::unordered_map<std::string, std::vector<HashLocation>>::iterator occurences_itr = occurences.begin();
        for (; occurences_itr != occurences.end(); ++occurences_itr) {

          // don't look for matches across submissions of the same student
          if (occurences_itr->first == (*submission_itr)->student()) {
            continue;
          }

          // save the locations of all other occurences of the matching hash in other students' submissions
          std::vector<HashLocation>::iterator itr = occurences_itr->second.begin();
          for (; itr != occurences_itr->second.end(); ++itr) {

            if (occurences.size() > (unsigned int) config.threshold) {
              // if the number of students with matching code is more
              // than the threshold, it is considered common code
              (*submission_itr)->addCommonMatch(hash_itr->second);
            } else {
              // save the match as a suspicous match
              (*submission_itr)->addSuspiciousMatch(hash_itr->second, *itr, hash_itr->first);
            }
          }
        }

        // look up the that hash in the other_gradeables table, loop over all other students that have the same hash
        std::unordered_map<std::string, std::vector<HashLocation>> other_occurences = other_gradeables[hash_itr->first];
        std::unordered_map<std::string, std::vector<HashLocation>>::iterator other_occurences_itr = other_occurences.begin();
        for (; other_occurences_itr != other_occurences.end(); ++other_occurences_itr) {

          // Note: we DO look for matches across submissions of the same student for self-plagiarism

          // save the locations of all other occurences from prior term submissions
          std::vector<HashLocation>::iterator itr = other_occurences_itr->second.begin();
          for (; itr != other_occurences_itr->second.end(); ++itr) {
            (*submission_itr)->addSuspiciousMatch(hash_itr->second, *itr, hash_itr->first);
          }
        }
      }
    }

    // If no suspicious matches were found, don't attempt to create any output files
    if ((*submission_itr)->getSuspiciousMatches().size() == 0) {
      delete (*submission_itr);
      (*submission_itr) = nullptr;
      continue;
    }

    // =========================================================================
    // Write matches.json file

    // holds the JSON file to be written
    std::vector<nlohmann::json> result;


    // **********  WRITE THE SUSPICIOUS MATCHES  **********
    // all of the suspicious matches for this submission
    std::map<location_in_submission, std::set<HashLocation> > suspicious_matches = (*submission_itr)->getSuspiciousMatches();

    // loop over each of the suspicious locations in the current submission
    for (std::map<location_in_submission, std::set<HashLocation> >::const_iterator location_itr
         =suspicious_matches.begin(); location_itr != suspicious_matches.end(); ++location_itr) {

      // stores matches of hash locations across other submssions in the class
      std::vector<nlohmann::json> others;

      {
        // generate a specific element of the "others" vector
        // set the variables to their initial values
        std::set<HashLocation>::const_iterator matching_positions_itr = location_itr->second.begin();
        nlohmann::json other;
        other["username"] = matching_positions_itr->student;
        other["version"] = matching_positions_itr->version;
        other["source_gradeable"] = matching_positions_itr->source_gradeable;
        std::vector<nlohmann::json> matchingpositions;
        nlohmann::json position;
        position["start"] = matching_positions_itr->location;
        position["end"] = matching_positions_itr->location + config.hash_size - 1;
        matchingpositions.push_back(position);

        // search for all matching positions of the suspicious match in other submissions
        if (location_itr->second.size() > 1) {
          ++matching_positions_itr;

          // loop over all of the other matching positions
          for (; matching_positions_itr != location_itr->second.end(); ++matching_positions_itr) {

            // keep iterating and editing the same object until a we get to a different submission
            if (matching_positions_itr->student != other["username"]
                || matching_positions_itr->version != other["version"]
                || matching_positions_itr->source_gradeable != other["source_gradeable"]
                || matchingpositions.size() >= lichen_config["max_matching_positions"]) {

              // found a different one, we push the old one and start over
              other["matchingpositions"] = matchingpositions;
              others.push_back(other);

              if (matchingpositions.size() >= lichen_config["max_matching_positions"]) {
                matching_positions_truncations[std::string(other["username"]) + std::string(":") + std::to_string(other["version"].get<int>())]++;
                break;
              }

              matchingpositions.clear();
              other["username"] = matching_positions_itr->student;
              other["version"] = matching_positions_itr->version;
              other["source_gradeable"] = matching_positions_itr->source_gradeable;
            }
            position["start"] = matching_positions_itr->location;
            position["end"] = matching_positions_itr->location + config.hash_size - 1;
            matchingpositions.push_back(position);
          }
        }

        other["matchingpositions"] = matchingpositions;
        others.push_back(other);
      }

      nlohmann::json info;
      info["start"] = location_itr->first;
      info["end"] = location_itr->first + config.hash_size - 1;
      info["type"] = "match";
      info["others"] = others;

      result.push_back(info);
    }
    // ****************************************************


    // ************* WRITE THE COMMON MATCHES *************
    // all of the common matches for this submission
    std::set<location_in_submission> common_matches = (*submission_itr)->getCommonMatches();
    for (std::set<location_in_submission>::const_iterator location_itr = common_matches.begin();
         location_itr != common_matches.end(); ++location_itr) {

      nlohmann::json info;
      info["start"] = *location_itr;
      info["end"] = *location_itr + config.hash_size - 1;
      info["type"] = "common";

      result.push_back(info);
    }
    // ****************************************************


    // *********** WRITE THE PROVIDED MATCHES *************
    // all of the provided code matches for this submission
    std::set<location_in_submission> provided_matches = (*submission_itr)->getProvidedMatches();
    for (std::set<location_in_submission>::const_iterator location_itr = provided_matches.begin();
         location_itr != provided_matches.end(); ++location_itr) {

      nlohmann::json info;
      info["start"] = *location_itr;
      info["end"] = *location_itr + config.hash_size - 1;
      info["type"] = "provided";

      result.push_back(info);
    }
    // ****************************************************


    // =========================================================================
    // Done creating the JSON file/objects, now we merge them to shrink them in size

    // Merge matching regions:
    if (!result.empty()) { // check to make sure that there are more than 1 positions (if it's 1, we can't merge anyway)
      // loop through all positions
      for (unsigned int position = 1; position < result.size(); position++) {
        nlohmann::json* prevPosition = &result[position - 1];
        nlohmann::json* currPosition = &result[position];
        // check whether they are next to each other and have the same type
        if ((*currPosition)["end"].get<int>() == (*prevPosition)["end"].get<int>() + 1 && (*currPosition)["type"] == (*prevPosition)["type"]) {
          bool canBeMerged = true;

          // if they are both of type match, we have to do extra steps to make sure they are mergeable
          if ((*prevPosition)["type"] == "match") {
            // easy check to see if they can't be merged for certain
            if ((*prevPosition)["others"].size() != (*currPosition)["others"].size()) {
              canBeMerged = false;
            }
            else {
              nlohmann::json::iterator prevPosItr = (*prevPosition)["others"].begin();
              nlohmann::json::iterator currPosItr = (*currPosition)["others"].begin();
              for (; prevPosItr != (*prevPosition)["others"].end() && currPosItr != (*currPosition)["others"].end(); prevPosItr++, currPosItr++) {
                // we can't merge the two positions if they are different in any way, except for the ending positions
                if ((*prevPosItr)["username"] != (*currPosItr)["username"] ||
                    (*prevPosItr)["version"] != (*currPosItr)["version"] ||
                    (*prevPosItr)["source_gradeable"] != (*currPosItr)["source_gradeable"] ||
                    !matchingPositionsAreAdjacent((*prevPosItr), (*currPosItr))) {
                  canBeMerged = false;
                  break;
                }
              }
            }
          }

          //if it's possible to do the merging, do it here by adjusting the end of the previous position and erasing the current position
          if (canBeMerged && (*prevPosition)["type"] == "match") {
            (*prevPosition)["end"] = (*currPosition)["end"].get<int>(); // (should be the equivalent of prevPosition["end"]++)

            // increment end positions for each element
            incrementEndPositionsForMatches((*prevPosition)["others"]);

            result.erase(result.begin() + position);
            position--;
          }
          else if (canBeMerged) { // common code or provided code that can be matched
            (*prevPosition)["end"] = (*currPosition)["end"].get<int>();

            result.erase(result.begin() + position);
            position--;
          }
        }
      }
    }

    // save the file with matches per user
    nlohmann::json match_data = result;
    boost::filesystem::path submission_dir = users_root_directory / (*submission_itr)->student() / std::to_string((*submission_itr)->version());
    boost::filesystem::create_directories(submission_dir);
    boost::filesystem::path matches_file = submission_dir / "matches.json";
    std::ofstream ostr(matches_file.string());
    assert(ostr.good());
    ostr << match_data.dump(4) << std::endl;

    // =========================================================================
    // Cleanup

    // Done with this submission. discard the data and clear the memory
    delete (*submission_itr);
    (*submission_itr) = nullptr;

    // Print current progress
    my_counter++;
    if (int((my_counter / float(all_submissions.size())) * 100) > my_percent) {
      int new_my_percent = int((my_counter / float(all_submissions.size())) * 100);
      for (int i=0; i < new_my_percent - my_percent; i++) {
        std::cout << "|";
      }
      fflush(stdout);
      my_percent = new_my_percent;
    }
  }

  // Finish printing any remaining portion of the progress bar
  for (int i=0; i < 100 - my_percent; i++) {
    std::cout << "|";
  }

  time(&end);
  diff = difftime(end, start);
  std::cout << "]" << std::endl;

  // Print out the list of users who had their matching positions array truncated
  if (matching_positions_truncations.size() > 0) {
    std::cout << "Matching positions array truncated for user(s): ";
    for (std::unordered_map<std::string, int>::const_iterator itr = matching_positions_truncations.begin();
        itr != matching_positions_truncations.end(); itr++) {
      std::cout << itr->first << " (" << itr->second << "), ";
    }
    std::cout << std::endl << "  - Try increasing the hash size or adding a regex to fix this problem." << std::endl;
  }

  // ===========================================================================
  // Done!

  time(&overall_end);
  double overall_diff = difftime(overall_end, overall_start);
  std::cout << "Hash comparison done in " << overall_diff << " seconds" << std::endl;
}
