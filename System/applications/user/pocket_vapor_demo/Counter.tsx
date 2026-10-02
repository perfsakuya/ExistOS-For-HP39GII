// Pocket Vapor proof of concept. The TypeScript is compiled on the PC;
// SkyOS runs only the generated C and a small native display adapter.
import { computed, ref } from "vue";
import { Button, onButton } from "./host/input.ts";
import { SCREEN } from "./host/screen.ts";

export default () => {
  const count = ref(0);
  const doubled = computed(() => count.value * 2);

  onButton((button) => {
    if (button === Button.Up || button === Button.Right) count.value++;
    if (button === Button.Down || button === Button.Left) count.value--;
    if (button === Button.A) count.value = 0;
  });

  return (
    <>
      <row y={0} class="bg-slate-100 text-slate-950 align-center">SKYOS UI TEST</row>
      <row y={3} x={1} class="text-white">Count: {count.value}</row>
      <row y={5} x={1} class="text-white">Double: {doubled.value}</row>
      <row y={SCREEN.height - 2} x={1} class="text-white">UP/DOWN +/- ENTER:RESET F6:EXIT</row>
    </>
  );
};
