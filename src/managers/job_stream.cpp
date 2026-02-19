#include "job_stream.hpp"

JobStream::JobStream(const std::string& job_id,
                     const std::string& marker_path)
    : job_id_(job_id), marker_path_(marker_path) {}
