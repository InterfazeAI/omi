/**
 * The worklets plugin is required by react-native-reanimated 4, which reaches
 * this project through react-native-audio-api's exported playback controls. It
 * must stay last in the plugin list.
 */
module.exports = function (api) {
  api.cache(true);
  return {
    presets: ['babel-preset-expo'],
    plugins: ['react-native-worklets/plugin'],
  };
};
