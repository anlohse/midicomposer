#include "project_document.hpp"

namespace midi_composer::project {

ProjectDocument::ProjectDocument(music::Composition composition) 
    : composition_(std::move(composition)) {}

} // namespace midi_composer::project
