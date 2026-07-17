import { ComponentFixture, TestBed } from '@angular/core/testing';

import { AboutComponent } from './about.component';

describe('AboutComponent', () => {
  let component: AboutComponent;
  let fixture: ComponentFixture<AboutComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      declarations: [AboutComponent],
    }).compileComponents();

    fixture = TestBed.createComponent(AboutComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });

  it('attributes the upstream projects and the GPL license', () => {
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('based on the open-source ESP-Miner and AxeOS projects');
    expect(text).toContain('ESP-Miner / AxeOS v2.14.2');
    expect(text).toContain('GNU General Public License v3.0');
  });

  it('names the supported target and the development status', () => {
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('Gamma board 601 / BM1370');
    expect(text).toContain('Development build');
    expect(text).toContain('NeuralShield');
  });

  it('claims no official Bitaxe affiliation', () => {
    const text = (fixture.nativeElement as HTMLElement).textContent ?? '';
    expect(text).toContain('not an official Bitaxe product');
  });

  it('keeps the Bitcoin whitepaper accessible', () => {
    const link = (fixture.nativeElement as HTMLElement).querySelector('a[href="/bitcoin.pdf"]');
    expect(link).toBeTruthy();
  });
});
