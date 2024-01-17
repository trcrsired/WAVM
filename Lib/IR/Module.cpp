#include "WAVM/IR/Module.h"
#include "WAVM/IR/IR.h"

using namespace WAVM;
using namespace WAVM::IR;

const char* IR::asString(OrderedSectionID id)
{
	switch(id)
	{
	case OrderedSectionID::moduleBeginning: return "module_ beginning";
	case OrderedSectionID::type: return "type";
	case OrderedSectionID::import_: return "import";
	case OrderedSectionID::function: return "func";
	case OrderedSectionID::table: return "table";
	case OrderedSectionID::memory: return "memory";
	case OrderedSectionID::global: return "global";
	case OrderedSectionID::tag: return "tag";
	case OrderedSectionID::export_: return "export";
	case OrderedSectionID::start: return "start";
	case OrderedSectionID::elem: return "elem";
	case OrderedSectionID::dataCount: return "data_count";
	case OrderedSectionID::code: return "code";
	case OrderedSectionID::data: return "data";

	default: WAVM_UNREACHABLE();
	};
}

bool IR::findCustomSection(const Module& module_,
						   const char* customSectionName,
						   Uptr& outCustomSectionIndex)
{
	for(Uptr sectionIndex = 0; sectionIndex < module_.customSections.size(); ++sectionIndex)
	{
		if(module_.customSections[sectionIndex].name == customSectionName)
		{
			outCustomSectionIndex = sectionIndex;
			return true;
		}
	}
	return false;
}

void IR::insertCustomSection(Module& module_, CustomSection&& customSection)
{
	auto it = module_.customSections.begin();
	for(; it != module_.customSections.end(); ++it)
	{
		if(it->afterSection > customSection.afterSection) { break; }
	}
	module_.customSections.insert(it, std::move(customSection));
}

OrderedSectionID IR::getMaxPresentSection(const Module& module_, OrderedSectionID maxSection)
{
	switch(maxSection)
	{
	case OrderedSectionID::data:
		if(hasDataSection(module_)) { return OrderedSectionID::data; }
		[[fallthrough]];
	case OrderedSectionID::code:
		if(hasCodeSection(module_)) { return OrderedSectionID::code; }
		[[fallthrough]];
	case OrderedSectionID::dataCount:
		if(hasDataCountSection(module_)) { return OrderedSectionID::dataCount; }
		[[fallthrough]];
	case OrderedSectionID::elem:
		if(hasElemSection(module_)) { return OrderedSectionID::elem; }
		[[fallthrough]];
	case OrderedSectionID::start:
		if(hasStartSection(module_)) { return OrderedSectionID::start; }
		[[fallthrough]];
	case OrderedSectionID::export_:
		if(hasExportSection(module_)) { return OrderedSectionID::export_; }
		[[fallthrough]];
	case OrderedSectionID::tag:
		if(hasTagSection(module_)) { return OrderedSectionID::tag; }
		[[fallthrough]];
	case OrderedSectionID::global:
		if(hasGlobalSection(module_)) { return OrderedSectionID::global; }
		[[fallthrough]];
	case OrderedSectionID::memory:
		if(hasMemorySection(module_)) { return OrderedSectionID::memory; }
		[[fallthrough]];
	case OrderedSectionID::table:
		if(hasTableSection(module_)) { return OrderedSectionID::table; }
		[[fallthrough]];
	case OrderedSectionID::function:
		if(hasFunctionSection(module_)) { return OrderedSectionID::function; }
		[[fallthrough]];
	case OrderedSectionID::import_:
		if(hasImportSection(module_)) { return OrderedSectionID::import_; }
		[[fallthrough]];
	case OrderedSectionID::type:
		if(hasTypeSection(module_)) { return OrderedSectionID::type; }
		[[fallthrough]];
	case OrderedSectionID::moduleBeginning: return OrderedSectionID::moduleBeginning;

	default: WAVM_UNREACHABLE();
	};
}
