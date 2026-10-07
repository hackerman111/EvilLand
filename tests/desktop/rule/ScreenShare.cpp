#include <desktop/rule/windowRule/WindowRule.hpp>
#include <desktop/rule/layerRule/LayerRule.hpp>
#include <render/ScreenShare.hpp>

#include <gtest/gtest.h>

using namespace Desktop::Rule;

TEST(ScreenShareRules, WindowRuleParsesIndependentBooleanEffects) {
    CWindowRule rule;
    ASSERT_TRUE(rule.addEffect(WINDOW_RULE_EFFECT_HIDE_FROM_SCREEN_SHARE, "on"));
    ASSERT_TRUE(rule.addEffect(WINDOW_RULE_EFFECT_NO_SCREEN_SHARE, "off"));
    ASSERT_EQ(rule.effects().size(), 2U);
    EXPECT_TRUE(std::get<bool>(rule.effects()[0].value));
    EXPECT_FALSE(std::get<bool>(rule.effects()[1].value));
    EXPECT_EQ(windowEffects()->get("hide_from_screen_share"), WINDOW_RULE_EFFECT_HIDE_FROM_SCREEN_SHARE);

    CWindowRule disabled;
    ASSERT_TRUE(disabled.addEffect(WINDOW_RULE_EFFECT_HIDE_FROM_SCREEN_SHARE, "off"));
    EXPECT_FALSE(std::get<bool>(disabled.effects()[0].value));
}

TEST(ScreenShareRules, LayerRuleParsesIndependentBooleanEffects) {
    CLayerRule rule;
    ASSERT_TRUE(rule.addEffect(LAYER_RULE_EFFECT_HIDE_FROM_SCREEN_SHARE, "on"));
    ASSERT_TRUE(rule.addEffect(LAYER_RULE_EFFECT_NO_SCREEN_SHARE, "off"));
    ASSERT_EQ(rule.effects().size(), 2U);
    EXPECT_TRUE(std::get<bool>(rule.effects()[0].value));
    EXPECT_FALSE(std::get<bool>(rule.effects()[1].value));
    EXPECT_EQ(layerEffects()->get("hide_from_screen_share"), LAYER_RULE_EFFECT_HIDE_FROM_SCREEN_SHARE);

    CLayerRule disabled;
    ASSERT_TRUE(disabled.addEffect(LAYER_RULE_EFFECT_HIDE_FROM_SCREEN_SHARE, "off"));
    EXPECT_FALSE(std::get<bool>(disabled.effects()[0].value));
}

TEST(ScreenShareRules, MissingSurfaceIsNotPrivate) {
    EXPECT_FALSE(Render::surfaceHiddenFromScreenShare(nullptr));
}
