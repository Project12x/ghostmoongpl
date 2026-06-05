#include <ghostmoon/ui/Geometry.h>
#include <gtest/gtest.h>

TEST(Geometry, KnobSweep270) {
    EXPECT_NEAR(gm::ui::knobSweepProportion(0.0f), 0.0f, 1e-5);
    EXPECT_NEAR(gm::ui::knobSweepProportion(1.0f), 1.0f, 1e-5);
    EXPECT_NEAR(gm::ui::knobSweepProportion(-0.1f), 0.0f, 1e-5);
    EXPECT_NEAR(gm::ui::knobSweepProportion( 1.1f), 1.0f, 1e-5);
}

TEST(Geometry, BipolarFillFromCenter) {
    EXPECT_NEAR(gm::ui::bipolarFill(0.5f).first,  0.5f, 1e-5);
    EXPECT_NEAR(gm::ui::bipolarFill(0.5f).second, 0.5f, 1e-5);
    auto [a, b] = gm::ui::bipolarFill(0.75f);
    EXPECT_NEAR(a, 0.5f,  1e-5);
    EXPECT_NEAR(b, 0.75f, 1e-5);
    { auto [a,b] = gm::ui::bipolarFill(0.25f); EXPECT_NEAR(a, 0.25f, 1e-5); EXPECT_NEAR(b, 0.5f, 1e-5); }
}
