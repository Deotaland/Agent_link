#pragma once
#include "application.h"

// Compatibility for existing board ports. New applications use Application.
using Board = Application;
#define DECLARE_BOARD DECLARE_APPLICATION
