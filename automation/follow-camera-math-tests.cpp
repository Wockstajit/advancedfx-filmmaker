#include "../AfxHookSource2/Filmmaker/Movie/FollowCameraMath.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace Filmmaker;

namespace {
void Check(bool condition, const char* message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

bool Near(double a, double b, double epsilon = 1e-6) {
	return std::fabs(a - b) <= epsilon;
}
}

int main() {
	Check(Near(FollowWrapDegrees(190.0), -170.0), "wrap +190");
	Check(Near(FollowWrapDegrees(-190.0), 170.0), "wrap -190");

	const FollowAngles east = FollowLookAt({0, 0, 0}, {100, 0, 0});
	Check(Near(east.pitch, 0.0) && Near(east.yaw, 0.0), "look east");
	const FollowAngles north = FollowLookAt({0, 0, 0}, {0, 100, 0});
	Check(Near(north.yaw, 90.0), "look north");
	const FollowAngles up = FollowLookAt({0, 0, 0}, {0, 0, 100});
	Check(Near(up.pitch, -90.0), "look up");

	Check(Near(FollowHalfTimeAlpha(1.0, 1.0), 0.5), "half-time alpha");
	const FollowAngles halfway = FollowSmoothAngles({0, 0, 0}, {0, 90, 0}, 1.0, 1.0, 0.0, 0.0);
	Check(Near(halfway.yaw, 45.0), "half-time angle smoothing");

	const FollowAngles wrapped = FollowSmoothAngles({0, 170, 0}, {0, -170, 0}, 1.0, 1.0, 0.0, 0.0);
	Check(Near(wrapped.yaw, 180.0), "shortest wrapped arc");

	const FollowAngles deadzone = FollowSmoothAngles({2, 3, 0}, {2.2, 3.2, 0}, 1.0, 0.1, 1.0, 0.0);
	Check(Near(deadzone.pitch, 2.0) && Near(deadzone.yaw, 3.0), "deadzone");

	const FollowAngles limited = FollowSmoothAngles({0, 0, 0}, {0, 180, 0}, 0.1, 0.0, 0.0, 30.0);
	Check(Near(std::fabs(limited.yaw), 3.0), "max turn speed");

	const FollowVec3 p = FollowSmoothPosition({0, 0, 0}, {10, 20, 30}, 1.0, 1.0);
	Check(Near(p.x, 5.0) && Near(p.y, 10.0) && Near(p.z, 15.0), "position smoothing");
	Check(Near(FollowDistance({0, 0, 0}, {3, 4, 0}), 5.0), "distance");

	const FollowVec3 front = FollowRotateVector({72, 0, 8}, {0, 0, 0});
	Check(Near(front.x, 72.0) && Near(front.y, 0.0) && Near(front.z, 8.0), "local front offset");
	const FollowVec3 yawedFront = FollowRotateVector({72, 0, 0}, {0, 90, 0});
	Check(Near(yawedFront.x, 0.0, 1e-5) && Near(yawedFront.y, 72.0, 1e-5), "local offset follows yaw");
	const FollowVec3 target{100, 20, 60};
	const FollowVec3 camera{172, 20, 68};
	const FollowAngles backAtTarget = FollowLookAt(camera, target);
	Check(Near(backAtTarget.yaw, 180.0) && backAtTarget.pitch > 0.0, "front camera looks back at target");

	const FollowVec3 frozen = FollowSmoothPosition({10, 20, 30}, {100, 200, 300}, 0.0, 0.25);
	Check(Near(frozen.x, 10.0) && Near(frozen.y, 20.0) && Near(frozen.z, 30.0), "zero-dt position hold");
	const FollowAngles frozenAngles = FollowSmoothAngles({4, 5, 6}, {40, 50, 60}, 0.0, 0.25, 0.0, 0.0);
	Check(Near(frozenAngles.pitch, 4.0) && Near(frozenAngles.yaw, 5.0), "zero-dt angle hold");

	std::cout << "FollowCameraMathTests: all checks passed\n";
	return 0;
}
